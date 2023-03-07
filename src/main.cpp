#define DEBUG
#define ON_LINUX
#ifdef ON_LINUX
#include <unistd.h>
#endif // ON_LINUX
#ifndef ON_LINUX
#include <Windows.h>
#endif // !ON_LINUX

// MiraiCP依赖文件(只需要引入这一个)
#include <MiraiCP.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
using namespace MiraiCP;
using namespace std;
bool ifexit = false;
mutex exitMtx;

namespace utils {

	void SleepForSeconds(unsigned long long sec) {
#ifdef ON_LINUX
		sleep(sec);
#endif // ON_LINUX
#ifndef ON_LINUX
		Sleep(sec * 1000);
#endif // !ON_LINUX
	}

	/*从一个MessageChain实例当中获取所有的文本内容并拼接为一个字符串*/
	string getAllPlainText(MessageChain* ch) {
		string ret = "";
		for (auto&& u : *ch) {
			if (u->internalType == SingleMessageType::PlainText_t) {
				ret += u->content;
			}
		}
		return ret;
	}

	/*从一个Json数据中获取所有的文本内容并拼接，效果同上*/
	string getAllPlainText(nlohmann::json json) {
		string ret = "";
		for (const auto& node : json) {
			if (node.contains("type") && node["type"] == "PlainText") {
				ret += node["content"];
			}
		}
		return ret;
	}

	bool deepSearchJson(const nlohmann::json _json, string key, string val) {
#ifdef DEBUG
		ofstream log_file;
		log_file.open("log.txt", ios_base::app);
		log_file << _json << endl;
		log_file.close();
		cout << "deepSearchJson...\n";
#endif
		bool ret = false;
		if (_json.contains(key) && _json[key] == val) {
			return true;
		}
		if (_json.is_array()) {
			for (auto& u : _json) {
				ret = ret || deepSearchJson(u, key, val);
			}
		}
		return ret;
	}

}  // namespace utils

// 独立的线程列表
vector<thread> threads;
MiraiCP::Bot* GlobalBot;
mutex GlobalBotMtx;
// 好友列表
vector<QQID> friend_list;
mutex friend_list_mtx;
struct PrivateChatInfo {
	QQID uid;
	int msgNum = 0;
	int responseNum = 0;
	unsigned long long last_time = 0;
	unsigned long long last_response = 0;
	PrivateChatInfo(QQID _uid) : uid(_uid) {}

	bool operator==(PrivateChatInfo& A) const { return this->uid == A.uid; }
	bool operator<(PrivateChatInfo& A) const {
		if (this->responseNum != A.responseNum) {
			return this->responseNum > A.responseNum;
		}
		if (this->msgNum != A.msgNum) {
			return this->msgNum < A.msgNum;
		}
		if (this->last_time != A.last_time) return this->last_time > A.last_time;
		if (this->last_response != A.last_response)
			return this->last_response > A.last_response;
		return this->uid > A.uid;
	}
	~PrivateChatInfo() {}
};
vector<PrivateChatInfo> friend_list_info;
mutex friend_list_info_mtx;
static unordered_map<QQID, QQID> chat_cp;
mutex chat_cp_mtx;

/*私聊功能*/
void privateChat() {
	Event::registerEvent<PrivateMessageEvent>([](PrivateMessageEvent e) {
		GlobalBotMtx.lock();
		GlobalBot = &e.bot;
		GlobalBotMtx.unlock();
		string msg = utils::getAllPlainText(e.getMessageChain());
		if (msg == "init" && e.sender.id() == 1924645279) {
			threads.emplace_back([]() {
				short STATE = 0;
				try {
#ifdef DEBUG
					cout << "Enter update friend_list thread...\n";
#endif
					while (true) {
#ifdef DEBUG
						cout << "Enter update friend_list thread loop...\n";
#endif
						exitMtx.lock();
						STATE = 3;
						if (ifexit) {
							exitMtx.unlock();
							STATE = 4;
							return;
						}
						exitMtx.unlock();
						STATE = 4;
						friend_list_mtx.lock();
						STATE = 5;
						friend_list_info_mtx.lock();
						STATE = 6;
						// 先更新好友列表
						cout << "Pointer : " << GlobalBot << endl;
						GlobalBotMtx.lock();
						friend_list = GlobalBot->getFriendList();
						GlobalBotMtx.unlock();
						// 不再是好友的直接移除列表
						unordered_map<QQID, bool> ifFriend;
						unordered_map<QQID, bool> addition;
						for (auto& u : friend_list) {
							ifFriend[u] = true;
						}
						vector<PrivateChatInfo> newArray;
						for (auto u : friend_list_info) {
							if (ifFriend[u.uid]) {
								newArray.push_back(u);
								addition[u.uid] = true;
							}
						}
						for (auto& [u, v] : ifFriend) {
							if (!addition[u]) {
								newArray.push_back(PrivateChatInfo(u));
							}
						}
						friend_list_info = newArray;
						sort(friend_list_info.begin(), friend_list_info.end());
						chat_cp_mtx.lock();
						STATE = 7;
						for (auto& u : friend_list_info) {
							if (chat_cp[u.uid] &&
								time(NULL) - u.last_response > 4 * 60 * 60) {
								QQID cp = chat_cp[u.uid];
								chat_cp[u.uid] = 0, chat_cp[cp] = 0;
							}
						}
						chat_cp_mtx.unlock();
						STATE = 8;
						friend_list_info_mtx.unlock();
						STATE = 9;
						friend_list_mtx.unlock();
						STATE = 10;
						utils::SleepForSeconds(6);
					}
				}
				catch (exception e) {
					cout << "Error occured in update friend_list thread...\n";
					cout << e.what() << endl;
					switch (STATE) {
					case 1:
						exitMtx.unlock();
						break;
					case 3:
						exitMtx.unlock();
						break;
					case 5:
						friend_list_mtx.unlock();
						break;
					case 6:
						friend_list_mtx.unlock(), friend_list_info_mtx.unlock();
						break;
					case 7:
						friend_list_mtx.unlock(), friend_list_info_mtx.unlock(),
							chat_cp_mtx.unlock();
						break;
					default:
						break;
					}
				}
				});
		}
		});

	Event::registerEvent<PrivateMessageEvent>([](PrivateMessageEvent e) {
		short STATE = 0;
		try {
			QQID uid = e.sender.id();
			friend_list_info_mtx.lock();
#ifdef DEBUG
			cout << "Length of friend_list: " << friend_list_info.size() << endl;
#endif
			STATE = 1;
			for (auto& node : friend_list_info) {
				if (node.uid == uid) {
					node.last_time = time(NULL);
					node.last_response = time(NULL);
					node.responseNum++;
					break;
				}
			}
			sort(friend_list_info.begin(), friend_list_info.end());
			friend_list_info_mtx.unlock();
			STATE = 2;
			chat_cp_mtx.lock();
			STATE = 3;
			// 私聊通话部分
			if (!chat_cp[uid]) {
				for (auto u : friend_list_info) {
					if (!chat_cp[u.uid] && u.uid != uid && u.uid != e.bot.botid()) {
						chat_cp[u.uid] = uid, chat_cp[uid] = u.uid;
						break;
					}
				}
			}
			if (chat_cp[uid]) {
				Friend _friend = Friend(chat_cp[uid], e.bot.id());
				friend_list_info_mtx.lock();
				STATE = 4;
				for (auto& u : friend_list_info) {
					if (u.uid == chat_cp[uid]) {
						u.msgNum++;
						break;
					}
				}
				friend_list_info_mtx.unlock();
				STATE = 5;

				nlohmann::json _json = e.getMessageChain()->toJson();
				bool isForwardMsg = utils::deepSearchJson(_json, "type", "ForwardMessage");
				if (isForwardMsg) {
					ForwardedMessage fmg = ForwardedMessage::deserializationFromMessageJson(_json);
					fmg.sendTo(&_friend);
				}
				else {
					_friend.sendMessage(e.message);
				}
			}

			chat_cp_mtx.unlock();
			STATE = 6;
		}
		catch (exception error) {
			cout << "Error occured in private chat...\n";
			cout << error.what() << endl;
			switch (STATE) {
			case 1:
				friend_list_info_mtx.unlock();
				break;
			case 3:
				chat_cp_mtx.unlock();
				break;
			case 4:
				friend_list_info_mtx.unlock(), chat_cp_mtx.unlock();
				break;
			case 5:
				chat_cp_mtx.unlock();
				break;
			default:
				break;
			}
		}
		});
	Event::registerEvent<GroupMessageEvent>([](GroupMessageEvent e) {
		GlobalBotMtx.lock();
		GlobalBot = &e.bot;
		GlobalBotMtx.unlock();
		short STATE = 0;
		try {
			friend_list_info_mtx.lock();
			STATE = 1;
			QQID uid = e.sender.id();
			for (auto& node : friend_list_info) {
				if (node.uid == uid) {
					node.last_time = time(NULL);
					break;
				}
			}
			sort(friend_list_info.begin(), friend_list_info.end());
			friend_list_info_mtx.unlock();
			STATE = 2;
		}
		catch (exception error) {
			cout << "Error occured in update active info...\n";
			cout << error.what() << endl;
			if (STATE == 1) {
				friend_list_info_mtx.unlock();
			}
		}
		});
}

const PluginConfig CPPPlugin::config{
	"YuYan",  // 插件id，如果和其他插件id重复将会被拒绝加载！
	"YuYanBot",  // 插件名称
	"1.0",       // 插件版本
	"DC233",     // 插件作者
	// "Plugin description", // 可选：插件描述
	// "Publish time"        // 可选：日期
};

// 插件实例
class PluginMain : public CPPPlugin {
public:
	// 配置插件信息
	PluginMain() : CPPPlugin() {}
	~PluginMain() override = default;  // override关键字是为了防止内存泄漏

	// 入口函数。插件初始化时会被调用一次，请在此处注册监听
	void onEnable() override {
		// 私聊功能注册
		privateChat();
	}

	// 退出函数。请在这里结束掉所有子线程，否则可能会导致程序崩溃
	void onDisable() override { /*插件结束前执行*/
#ifdef DEBUG
		cout << "It's going to disabling plugin...\n";
#endif
		exitMtx.lock();
		ifexit = true;
		exitMtx.unlock();
#ifdef DEBUG
		cout << "Update flag of exit sucessfully...\n";
#endif
		for (auto& u : threads) {
			u.join();
		}

#ifdef DEBUG
		cout << "All threads has been close, process exit...\n";
#endif
	}
};

// 创建当前插件实例。请不要进行其他操作，
// 初始化请在onEnable中进行
void MiraiCP::enrollPlugin() { MiraiCP::enrollPlugin<PluginMain>(); }
