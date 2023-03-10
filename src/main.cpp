#define DEBUG
// #define ON_LINUX
#ifdef ON_LINUX
#include <unistd.h>
#endif  // ON_LINUX
#ifndef ON_LINUX
#include <Windows.h>
#endif  // !ON_LINUX

// MiraiCP依赖文件(只需要引入这一个)
#include <MiraiCP.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <stack>
using namespace MiraiCP;
using namespace std;

const PluginConfig CPPPlugin::config{
	"YuYan",  // 插件id，如果和其他插件id重复将会被拒绝加载！
	"YuYanBot",  // 插件名称
	"1.2",       // 插件版本
	"DC233",     // 插件作者
};

struct FriendActivation {
	QQID uid;      // 好友ID
	int priority;  // 活跃权重
	FriendActivation(QQID id) { this->uid = id, this->priority = 0; }
	FriendActivation(QQID id, int pri) { this->uid = id, this->priority = pri; }
	~FriendActivation() {}
	bool operator<(const FriendActivation& A) const {
		return this->priority > A.priority;
	}
};

struct RandChatInfo {
	string realId;//消息的实际来源id
	QQID realGroup;//消息的实际来源群
	MessageSource realSource;//消息的实际源
	string virtualId;//机器人转发后的消息id
	QQID virtualGroup;//机器人转发后的群
	RandChatInfo(string _realId, QQID _realGroup,
		MessageSource _realSource,
		string _virtualId,
		QQID _virtualGroup
	) {
		this->realId = _realId;
		this->realGroup = _realGroup;
		this->realSource = _realSource;
		this->virtualId = _virtualId;
		this->virtualGroup = _virtualGroup;
	}
	~RandChatInfo() {}
};

/********************* 公共资源区  ************************/

bool ifexit = false;  // 退出标记，true意味着程序即将退出
mutex exitMtx;
const QQID bot_id = 1784518480;
vector<QQID> friendList;  // 机器人的好友列表
vector<QQID> groupList;		//机器人的群列表
mutex friendListMutex;
mutex groupListMutex;
vector<FriendActivation> activation;  // 好友活跃度列表
mutex activationMutex;
unordered_map<QQID, QQID> chatCP;  // 好友对话对象
mutex chatCPMutex;
unordered_map<QQID, unsigned long long>lastResponseTime;//用户最后一次和Bot发私信的时间戳
mutex lastResponseTimeMutex;
vector<RandChatInfo>randChatInfoList;//记录bot转发的消息列表
mutex randChatInfoListMutex;
const size_t randChatInfoListMaxLength = 10000;
const vector<string> queryWords = { "请问","为什么","有谁知道","吗?", "吗？" };

/*********************************************************/

namespace utils {

	optional<RandChatInfo> getChatInfoWithQuote(
		QQID& quotedGroup, string& quotedId
	) {
		randChatInfoListMutex.lock();
		for (auto& u : randChatInfoList) {
			if (u.virtualGroup == quotedGroup
				&& u.virtualId == quotedId) {
				randChatInfoListMutex.unlock();
				return optional<RandChatInfo>(u);
			}
		}
		randChatInfoListMutex.unlock();
		return optional<RandChatInfo>();
	}

	optional<QQID> getRandGroupExceptList(vector<QQID> exceptedList) {
#ifdef DEBUG
		cout << "Enter GetRandGroupExceptList..." << endl;
#endif
		vector<QQID> tempList;
		unordered_map<QQID, bool> excepted;
		for (auto& u : exceptedList) {
			excepted[u] = true;
		}
		groupListMutex.lock();
		for (auto& u : groupList) {
#ifdef DEBUG
			cout << u << " ";
#endif
			if (!excepted[u])tempList.push_back(u);
		}
#ifdef DEBUG
		cout << endl;
#endif
		groupListMutex.unlock();
		if (tempList.empty())return optional<QQID>();
		return optional<QQID>(tempList[rand() % tempList.size()]);
	}

	void addRandChatInfo(RandChatInfo& info) {
		randChatInfoListMutex.lock();
		randChatInfoList.push_back(info);
		if (randChatInfoList.size() > randChatInfoListMaxLength) {
			randChatInfoList.erase(randChatInfoList.begin());
		}
		randChatInfoListMutex.unlock();
	}

	MessageChain messageChainAdd(MessageChain& origin, MessageChain& add) {
		nlohmann::json _retJson = origin.toJson();
		nlohmann::json _json = add.toJson();
		for (auto& u : _json) {
			_retJson.push_back(u);
		}
		return MessageChain::deserializationFromMessageJson(_retJson);
	}

	MessageChain makeVirtualQuote(QQID uid, QQID group, unsigned long long _time, MessageChain& origin, MessageChain& reply) {
		nlohmann::json _json = nlohmann::json::parse(
			R"(
			[
			  {
				"source": {
				  "botId": 3278555283,
				  "fromId": 1924645279,
				  "ids": [
					0
				  ],
				  "internalIds": [
					1739463707
				  ],
				  "kind": "GROUP",
				  "originalMessage": [],
				  "targetId": 261925581,
				  "time": 1678443847
				},
				"type": "QuoteReply"
			  }
			]
		)"
		);
		_json[0]["source"]["botId"] = bot_id;
		_json[0]["source"]["fromId"] = bot_id;
		_json[0]["source"]["time"] = _time;
		_json[0]["source"]["group"] = group;
		_json[0]["source"]["originalMessage"] = origin.toJson();
		nlohmann::json _tempJson = reply.toJson();
		for (auto& u : _tempJson) {
			_json.push_back(u);
		}
		MessageChain ret = MessageChain::deserializationFromMessageJson(_json);
		return ret;
	}

	void setLastResponseTime(QQID uid, unsigned long long _time) {
		lastResponseTimeMutex.lock();
		lastResponseTime[uid] = _time;
		lastResponseTimeMutex.unlock();
	}

	unsigned long long getLastResponseTime(QQID uid) {
		unsigned long long ret;
		lastResponseTimeMutex.lock();
		ret = lastResponseTime[uid];
		lastResponseTimeMutex.unlock();
		return ret;
	}

	QQID getAChatCP(QQID uid) {
		chatCPMutex.lock();
		if (chatCP[uid]) {
			chatCPMutex.unlock();
			return chatCP[uid];
		}
		activationMutex.lock();
		for (auto& u : activation) {
			if (!chatCP[u.uid] && u.uid != uid && u.uid != bot_id) {
				chatCP[uid] = u.uid;
				chatCP[u.uid] = uid;
				break;
			}
		}
		activationMutex.unlock();
		chatCPMutex.unlock();
		return chatCP[uid];
	}

	void activationAddition(QQID uid, int pri) {
		activationMutex.lock();
		size_t number = activation.size();
		size_t i;
		for (i = 0; i < number; i++) {
			if (activation[i].uid == uid) break;
		}
		if (i < number) {
			activation[i].priority += pri;
			while (i && activation[i].priority > activation[i - 1].priority) {
				swap(activation[i], activation[i - 1]);
				i--;
			}
			while (i + 1 < number &&
				activation[i].priority < activation[i + 1].priority) {
				swap(activation[i], activation[i + 1]);
				i++;
			}
		}
		activationMutex.unlock();
	}

	bool checkIsFriend(QQID uid) {
		friendListMutex.lock();
		for (auto& u : friendList) {
			if (u == uid) {
				friendListMutex.unlock();
				return true;
			}
		}
		friendListMutex.unlock();
		return false;
	}

	vector<QQID> getFriendList() {
		vector<QQID> ret;
		friendListMutex.lock();
		ret = friendList;
		friendListMutex.unlock();
		return ret;
	}

	void updateFriendList(vector<QQID> new_list) {
		friendListMutex.lock();
		friendList = new_list;
		friendListMutex.unlock();
	}

	void updateGroupList(vector<QQID> new_list) {
		groupListMutex.lock();
		groupList = new_list;
		groupListMutex.unlock();
	}

	bool checkIfExit() {
		exitMtx.lock();
		bool ret = ifexit;
		exitMtx.unlock();
		return ret;
	}

	void setExit() {
		exitMtx.lock();
		ifexit = true;
		exitMtx.unlock();
	}

	void SleepForSeconds(unsigned long long sec) {
#ifdef ON_LINUX
		sleep(sec);
#endif  // ON_LINUX
#ifndef ON_LINUX
		Sleep(sec * 1000);
#endif  // !ON_LINUX
	}

	/* 从一个MessageChain实例当中获取所有的文本内容并拼接为一个字符串 */
	string getAllPlainText(const MessageChain* ch) {
		string ret = "";
		for (const auto& u : *ch) {
			if (u->internalType == SingleMessageType::PlainText_t) ret += u->content;
		}
		return ret;
	}

	/* 从一个Json数据中获取所有的文本内容并拼接，效果同上 */
	string getAllPlainText(const nlohmann::json json) {
		string ret = "";
		for (const auto& node : json) {
			if (node.contains("type") && node["type"] == "PlainText")
				ret += node["content"];
		}
		return ret;
	}

	/* 深搜检查某个JSON是否存在一个<key, val>键值对 */
	template <typename T>
	bool deepCheckKeyValFromJson(const nlohmann::json _json, string key, T val) {
#ifdef DEBUG
		cout << "Enter deepCheckKeyValFromJson..." << endl;
		cout << "当前要查找的Key: " << key << endl;
		cout << "当前要查找的Val: " << val << endl;
		cout << "当前要查找的JSON: " << _json << endl;
#endif
		bool ret = false;
		if (_json.is_object()) { // 如果是对象
			for (auto it = _json.begin(); it != _json.end(); ++it) {
				if (it.key() == key && it.value().get<T>() == val) {
					return true;
				}
				else if (it.value().is_structured()) {
					ret |= deepCheckKeyValFromJson<T>(it.value(), key, val);
				}
			}
		}
		else if (_json.is_array()) { // 如果是数组
			for (auto& u : _json) {
				//if (u.is_object() || u.is_structured()) {
				ret |= deepCheckKeyValFromJson<T>(u, key, val);
				//}
			}
		}
		return ret;
	}


	/* 深搜某个JSON中key的val */
	template <typename T>
	optional<T> deepGetValFromJson(const nlohmann::json _json, string key) {
#ifdef DEBUG
		cout << "Enter deepGetValFromJson..." << endl;
		cout << "当前要查找的Key: " << key << endl;
		cout << "当前要查找的JSON: " << _json << endl;
#endif
		if (_json.is_object()) {
			for (auto it = _json.begin(); it != _json.end(); ++it) {
				if (it.key() == key) {
					return optional<T>(it.value());
				}
				else if (it.value().is_structured()) {
					optional<T> nested = deepGetValFromJson<T>(it.value(), key);
					if (nested.has_value()) {
						return nested;
					}
				}
			}
		}
		else if (_json.is_array()) {
			for (auto& u : _json) {
				optional<T> nested = deepGetValFromJson<T>(u, key);
				if (nested.has_value()) {
					return nested;
				}
			}
		}
		return optional<T>();
	}

	/* 深搜查找并返回存在某个key的JSON */
	optional<nlohmann::json> deepSearchJsonWithKey(const nlohmann::json _json, string key) {
#ifdef DEBUG
		cout << "Enter deepSearchJsonWithKey..." << endl;
		cout << "当前要查找的Key: " << key << endl;
		cout << "当前要查找的JSON: " << _json << endl;
#endif
		if (_json.is_object()) {
			for (auto it = _json.begin(); it != _json.end(); ++it) {
				if (it.key() == key) {
					return optional<nlohmann::json>(_json);
				}
				else if (it.value().is_structured()) {
					optional<nlohmann::json> nested = deepSearchJsonWithKey(it.value(), key);
					if (nested.has_value()) {
						return nested;
					}
				}
			}
		}
		else if (_json.is_array()) {
			for (auto& u : _json) {
				optional<nlohmann::json> nested = deepSearchJsonWithKey(u, key);
				if (nested.has_value()) {
					return nested;
				}
			}
		}
		return optional<nlohmann::json>();
	}

	/* 深搜查找并返回存在某个<key, val>的JSON */
	template<typename T>
	optional<nlohmann::json> deepSearchJsonWithKeyVal(const nlohmann::json _json, string key, T val) {
#ifdef DEBUG
		cout << "Enter deepSearchJsonWithKeyVal..." << endl;
		cout << "当前要查找的Key: " << key << endl;
		cout << "当前要查找的Val: " << val << endl;
		cout << "当前要查找的JSON: " << _json << endl;
#endif
		if (_json.is_object()) {
			for (auto it = _json.begin(); it != _json.end(); ++it) {
				if (it.key() == key && it.value() == val) {
					return optional<nlohmann::json>(_json);
				}
				else if (it.value().is_structured()) {
					optional<nlohmann::json> nested = deepSearchJsonWithKeyVal<T>(it.value(), key, val);
					if (nested.has_value()) {
						return nested;
					}
				}
			}
		}
		else if (_json.is_array()) {
			for (auto& u : _json) {
				optional<nlohmann::json> nested = deepSearchJsonWithKeyVal<T>(u, key, val);
				if (nested.has_value()) {
					return nested;
				}
			}
		}
		return optional<nlohmann::json>();
	}

}  // namespace utils

/* 一些初始化 */
void init() {
#ifndef ON_LINUX
	system("chcp 65001");
#endif
}

/**
 * @brief  周期性地更新好友列表的函数，应当作为一个独立的线程运行
 * @param  period_sec 周期，单位为秒
 * @return
 */
void updateFriendListPeriodicallyThread(const unsigned int period_sec) {
	utils::SleepForSeconds(20);
	while (true) {
#ifdef DEBUG
		cout << "Update FriendList Thread is running..." << endl;
#endif
		if (utils::checkIfExit()) {
			cout << "Update FriendList Thread is closing..." << endl;
			return;
		}
		MiraiCP::Bot _bot(bot_id);
		vector<QQID> tempList = _bot.getFriendList();
		utils::updateFriendList(tempList);
		vector<QQID> tempGroupList = _bot.getGroupList();
		utils::updateGroupList(tempGroupList);
		unordered_map<QQID, bool> isFriend, addition;
		vector<FriendActivation> tempActivation;
		for (auto& u : tempList) isFriend[u] = true;
		activationMutex.lock();
		for (auto& u : activation) {
			if (isFriend[u.uid]) {
				tempActivation.push_back(u);
				addition[u.uid] = true;
			}
		}
		for (auto& [u, v] : isFriend) {
			if (v && !addition[u]) tempActivation.push_back(FriendActivation(u));
		}
		activation = tempActivation;
#ifdef DEBUG
		cout << "FriendList Length is : " << activation.size() << endl;
		cout << "GroupList Length is : " << tempGroupList.size() << endl;
#endif
		activationMutex.unlock();
#ifdef DEBUG
		cout << "Update FriendList Thread finish a period..." << endl;
#endif
		utils::SleepForSeconds(period_sec);
	}
}

/**
 * @brief  周期性地更新好友活跃度
 * @param  period_sec 周期,单位为秒
 * @return
 */
void updateActivationPeriodcallyThread(const unsigned int period_sec) {
	while (true) {
		if (utils::checkIfExit()) {
			cout << "Update Activation Thread is closing..." << endl;
			return;
		}
#ifdef DEBUG
		cout << "Update Activation Thread is running..." << endl;
#endif
		activationMutex.lock();
		for (auto& [u, v] : activation) v /= 2;
		activationMutex.unlock();
#ifdef DEBUG
		cout << "Update Activation a period done..." << endl;
#endif
		utils::SleepForSeconds(period_sec);
	}
}

/**
* @brief  周期性地解除私信对话超时的双方联系
* @param  period_sec  周期，单位为秒
* @param  limit_sec   超时判定的界限，单位为秒
* @return
*/
void updateChatCPInfoThread(const unsigned int period_sec, const unsigned long long limit_sec) {
	while (true) {
		if (utils::checkIfExit()) {
#ifdef DEBUG
			cout << "Update ChatCP Thread is closing..." << endl;
#endif
			return;
		}
		chatCPMutex.lock();
		unsigned long long now = time(NULL);
		for (auto& [u, v] : chatCP) {
			if (v && now - utils::getLastResponseTime(v) > limit_sec) {
				chatCP[v] = 0, chatCP[u] = 0;
			}
		}
		chatCPMutex.unlock();
		utils::SleepForSeconds(period_sec);
	}
}

// 记录好友的活跃度
void collectFriendActivation() {
	MiraiCP::MiraiCPNewThread(updateActivationPeriodcallyThread, 600).detach();
	Event::registerEvent<PrivateMessageEvent>([](PrivateMessageEvent e) {
#ifdef DEBUG
		cout << "由私信激发的活跃即将更新..." << endl;
#endif
		QQID uid = e.sender.id();
		// #ifdef DEBUG
		//     cout << "将在10秒后更新活跃权重..." << endl;
		// #endif
		//     utils::SleepForSeconds(10);
		utils::activationAddition(uid, 5);
#ifdef DEBUG
		cout << "由私信激发的活跃更新完毕..." << endl;
#endif
		});
	Event::registerEvent<GroupMessageEvent>([](GroupMessageEvent e) {
		QQID uid = e.sender.id();
		utils::activationAddition(uid, 1);
		});
	Event::registerEvent<GroupMessageEvent>([](GroupMessageEvent e) {
		string msg = utils::getAllPlainText(e.getMessageChain());
		if (msg == "rank") {
			string ret = "--------------------\n";
			activationMutex.lock();
			size_t i, number = activation.size();
			for (i = 0; i < min((size_t)10, number); i++) {
				ret += to_string(activation[i].uid) + "\t" +
					to_string(activation[i].priority) + "\n";
			}
			activationMutex.unlock();
			e.group.sendMessage(ret);
		}
		});
}

// 私聊系统
void privateChatSystem() {
	//  添加定时更新好友列表的线程
	MiraiCP::MiraiCPNewThread(updateFriendListPeriodicallyThread,
		120).detach();
	//  评估/维护好友的活跃度
	collectFriendActivation();
	//  定时解除过时CP线程
	MiraiCP::MiraiCPNewThread(updateChatCPInfoThread, 600, 14400).detach();

	Event::registerEvent<PrivateMessageEvent>([](PrivateMessageEvent e) {
		try {
#ifdef DEBUG
			cout << "正在执行私聊系统线程..." << endl;
#endif
#ifdef DEBUG
			cout << "私聊系统开始开始匹配..." << endl;
#endif
			QQID uid = e.sender.id();
			QQID to_uid = utils::getAChatCP(uid);
			nlohmann::json _json = e.message.toJson();
			//QQID to_uid = uid ^ 1924645279 ^ 783371620;
#ifdef DEBUG
			cout << "Get to_send_uid : " << to_uid << endl;
#endif
			if (to_uid) {
#ifdef DEBUG
				cout << "即将转发消息..." << endl;
#endif
				utils::activationAddition(uid, 5);
				utils::activationAddition(to_uid, -5);
#ifdef DEBUG
				cout << "更新活跃度完毕..." << endl;
#endif
				Friend _fri = Friend(to_uid, bot_id);
#ifdef DEBUG
				cout << "构造好友实例完毕..." << endl;
#endif
				if (utils::deepCheckKeyValFromJson<string>(_json, "type", "ForwardMessage")) {
#ifdef DEBUG
					cout << "开始检索JSON数据..." << endl;
#endif
					optional<nlohmann::json> tempJson = utils::deepSearchJsonWithKeyVal<string>(
						_json, "type", "ForwardMessage");
#ifdef DEBUG
					cout << "检索JSON数据完毕..." << endl;
#endif
					if (tempJson.has_value()) {
						ForwardedMessage::deserializationFromMessageJson(
							tempJson.value()
						).sendTo(&_fri);
					}
				}
				else { _fri.sendMessage(e.message); }

#ifdef DEBUG
				cout << "转发消息成功..." << endl;
#endif
			}
#ifdef DEBUG
			cout << "正常执行完一次私聊转发..." << endl;
#endif
		}
		catch (exception error) {}
		});
}

// 胡言乱语系统
void randChatSystem() {
	//引用转发
	Event::registerEvent<GroupMessageEvent>([](GroupMessageEvent e) {
		try {
			nlohmann::json _json = e.message.toJson();
			if (!utils::deepCheckKeyValFromJson<string>(_json, "type", "QuoteReply"))
				return;
#ifdef DEBUG
			cout << "检测到一条引用消息..." << endl;
#endif
			optional<nlohmann::json> tempJson = utils::deepSearchJsonWithKey(_json, "source");
			if (!tempJson.has_value())return;
			optional<QQID>_uid = utils::deepGetValFromJson<QQID>(tempJson.value(), "fromId");
			optional<nlohmann::json> _id = utils::deepGetValFromJson<nlohmann::json>(tempJson.value(), "ids");
#ifdef DEBUG
			cout << "引用的 消息id 是否被检测到: " << _id.has_value() << endl;
#endif
			if (!_uid.has_value() || !_id.has_value())return;
#ifdef DEBUG
			cout << "正在构建待存储消息实例..." << endl;
#endif 
			QQID _group = e.group.id();
			optional<RandChatInfo> _info = utils::getChatInfoWithQuote(_group, 
				"[" + to_string(_id.value()[0]) + "]");
			if (!_info.has_value())return;
#ifdef DEBUG
			cout << "即将转发引用消息..." << endl;
#endif
			//应当转发消息
			Group group(_info.value().realGroup, bot_id);
			MessageSource _source = group.quoteAndSendMessage(_info.value().realSource, e.message);
#ifdef DEBUG
			cout << "转发引用消息完毕..." << endl;
#endif
			//存储本条消息
			RandChatInfo _newInfo(
				e.message.source.value().ids,
				_group, e.message.source.value(),
				_source.ids,
				_info.value().realGroup
			);
			utils::addRandChatInfo(_newInfo);
#ifdef DEBUG
			cout << "存储转发消息完毕..." << endl;
#endif
		}
		catch (exception error) {}
		});

	//提问句式转发
	Event::registerEvent<GroupMessageEvent>([](GroupMessageEvent e) {
		try {
			string text = utils::getAllPlainText(e.message.toJson());
			bool isQuery = false;
			for (auto& word : queryWords) {
				if (text.find(word) != string::npos) {
					isQuery = true;
					break;
				}
			}
			if (!isQuery) return;
#ifdef DEBUG
			cout << "检测到询问语句..." << endl;
#endif
			QQID _realGroup = e.group.id();
#ifdef DEBUG
			cout << "_realGroup : " << _realGroup << endl;
#endif
			MessageSource _source = e.message.source.value();
			string _realId = _source.ids;
#ifdef DEBUG
			cout << "_realId : " << _realId << endl;
#endif
			optional<QQID> _virtualGroup = utils::getRandGroupExceptList({ _realGroup });
#ifdef DEBUG
			cout << "_virtualGroup-hasValue : " << _virtualGroup.has_value() << endl;
#endif
			if (!_virtualGroup.has_value())return;
			Group group(_virtualGroup.value(), bot_id);
			MessageSource source = group.sendMessage(e.message);
#ifdef DEBUG
			cout << "已将提问随机发送到群: " << group.id() << endl;
#endif
			string _virtualId = source.ids;
			RandChatInfo new_one(_realId, _realGroup, _source, _virtualId, _virtualGroup.value());
			utils::addRandChatInfo(new_one);
#ifdef DEBUG
			cout << "已将该提问消息列入列表..." << endl;
			cout << "其realId为: " << _realId << endl;
			cout << "其realGroup为: " << _realGroup << endl;
			cout << "其virtualId为: " << _virtualId << endl;
			cout << "其virtualGroup为: " << _virtualGroup.value() << endl;
#endif
		}
		catch (exception error) {}
		});
}

// 插件实例
class PluginMain : public CPPPlugin {
public:
	// 配置插件信息
	PluginMain() : CPPPlugin() {}
	~PluginMain() override = default;  // override关键字是为了防止内存泄漏

	// 入口函数。插件初始化时会被调用一次，请在此处注册监听
	void onEnable() override {
		init();

		// 私聊系统
		privateChatSystem();

		// 胡言乱语
		randChatSystem();

		/******************** 测试区域 *********************/
		Event::registerEvent<GroupMessageEvent>([](GroupMessageEvent e) {

			});
	}

	// 退出函数。请在这里结束掉所有子线程，否则可能会导致程序崩溃
	void onDisable() override { /*插件结束前执行*/
#ifdef DEBUG
		cout << "It's going to disabling plugin...\n";
#endif
		utils::setExit();
#ifdef DEBUG
		cout << "Update flag of exit sucessfully...\n";
		cout << "All threads has been close, process exit...\n";
#endif
	}
};

// 创建当前插件实例。请不要进行其他操作，
// 初始化请在onEnable中进行
void MiraiCP::enrollPlugin() { MiraiCP::enrollPlugin<PluginMain>(); }
