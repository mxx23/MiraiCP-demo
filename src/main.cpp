#define DEBUG
#define ON_LINUX
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
    "1.0",       // 插件版本
    "DC233",     // 插件作者
};

struct FriendActivation {
  QQID uid;      // 好友ID
  int priority;  // 活跃权重
  FriendActivation(QQID id) { this->uid = id, this->priority = 0; }
  FriendActivation(QQID id, int pri) { this->uid = id, this->priority = pri; }
  bool operator<(const FriendActivation& A) const {
    return this->priority > A.priority;
  }
  ~FriendActivation() {}
};

/********************* 公共资源区  ************************/

bool ifexit = false;  // 退出标记，true意味着程序即将退出
mutex exitMtx;
const QQID bot_id = 3278555283;
vector<QQID> friendList;  // 机器人的好友列表
mutex friendListMutex;
vector<FriendActivation> activation;  // 好友活跃度列表
mutex activationMutex;
unordered_map<QQID, QQID> chatCP;  // 好友对话对象
mutex chatCPMutex;

/*********************************************************/

namespace utils {

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
  for (auto&& u : *ch) {
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
bool deepSearchJson(const nlohmann::json _json, string key, T val) {
  bool ret = false;
  if (_json.contains(key) && _json[key].get<T>() == val) return true;
  if (_json.is_array())
    for (auto& u : _json) ret = ret || deepSearchJson(u, key, val);
  return ret;
}

}  // namespace utils

/**
 * @brief  周期性地更新好友列表的函数，应当作为一个独立的线程运行
 * @param  period_sec 周期，单位为秒
 * @return
 */
void updateFriendListPeriodicallyThread(const unsigned int period_sec) {
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

// 记录好友的活跃度
void collectFriendActivation() {
  MiraiCP::MiraiCPNewThread(updateActivationPeriodcallyThread, 600).detach();
  Event::registerEvent<PrivateMessageEvent>([](PrivateMessageEvent e) {
    QQID uid = e.sender.id();
    utils::activationAddition(uid, 5);
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
  Event::registerEvent<PrivateMessageEvent>([](PrivateMessageEvent e) {
    try {
      QQID uid = e.sender.id();
      QQID to_uid = utils::getAChatCP(uid);
#ifdef DEBUG
      cout << "Get to_send_uid : " << to_uid << endl;
#endif
      if (to_uid) {
        utils::activationAddition(uid, 5);
        utils::activationAddition(to_uid, -5);
        Friend _fri = Friend(to_uid, bot_id);
        _fri.sendMessage(e.message);
      }
    } catch (exception error) {
      cout << "PrivateChatSystem Error..." << endl;
      cout << error.what() << endl;
    }
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
    // 添加定时更新好友列表的线程
    MiraiCP::MiraiCPNewThread(updateFriendListPeriodicallyThread, 120).detach();
    // 评估/维护好友的活跃度
    collectFriendActivation();
    // 私聊系统
    privateChatSystem();
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
