/**
 * ============================================================================
 * 中介者模式 (Mediator Pattern) - 行为型设计模式
 * ============================================================================
 *
 * 【解决的问题】
 *
 * 当系统中多个对象之间存在复杂的交互关系时，如果让它们直接互相引用
 * 和通信，会形成"网状依赖"：
 *
 *   - 每个对象都需要知道其他所有对象的引用
 *   - 修改一个对象的交互逻辑会影响其他对象
 *   - 新增对象需要修改所有相关对象
 *   - 系统难以理解和维护
 *
 * 中介者模式引入一个"中介者"对象来集中管理所有交互逻辑，把多对多的
 * 网状通信变成"星形结构"（所有对象只与中介者通信）。
 *
 * 【核心思想】
 *
 *   - Mediator（中介者）：定义与 Colleague 通信的接口
 *   - ConcreteMediator：实现具体的交互逻辑
 *   - Colleague（同事）：各业务对象，只通过中介者与其他同事通信
 *
 *   交互前:  A <-> B, A <-> C, B <-> C  (网状)
 *   交互后:  A -> Mediator <- B, C       (星形)
 *
 * 【典型场景】
 *   - 聊天室（用户之间通过聊天室通信，不直接互相引用）
 *   - 机场塔台（飞机之间通过塔台协调，不直接对话）
 *   - 表单控件联动（输入框变化触发按钮状态、标签更新）
 *
 * 【与组合模式、桥接模式的区别】
 *
 *   中介者模式 (Mediator):
 *     - 行为型模式
 *     - 解决「多对象网状通信」问题
 *     - 引入中介者对象集中管理交互逻辑
 *     - 关注：解耦对象间的通信关系
 *
 *   组合模式 (Composite):
 *     - 结构型模式
 *     - 解决「树形结构中统一操作」问题
 *     - 叶子和容器共享接口，递归组合
 *     - 关注：统一处理部分-整体层次
 *
 *   桥接模式 (Bridge):
 *     - 结构型模式
 *     - 解决「多维度类爆炸」问题
 *     - 拆分抽象和实现到两个继承体系
 *     - 关注：解耦抽象与实现的变化
 *
 * 【一句话总结】
 *   中介 → 收拢网状通信，消灭对象间的多对多依赖
 *   组合 → 统一树形操作，叶子和容器一个接口
 *   桥接 → 拆分多维变化，组合替代继承
 *
 * ============================================================================
 */

#include <iostream>
#include <string>
#include <vector>

class ChatRoom;

class User {
public:
    explicit User(std::string name) : name_(std::move(name)) {}

    const std::string& getName() const { return name_; }
    void setRoom(ChatRoom* room) { room_ = room; }

    void sendMessage(const std::string& message) const;
    void sendPrivateMessage(const std::string& to, const std::string& message) const;
    void receive(const std::string& from, const std::string& message) const;

private:
    std::string name_;
    ChatRoom* room_ = nullptr;
};

class ChatRoom {
public:
    void addUser(User* user) {
        users_.push_back(user);
        user->setRoom(this);
        broadcast("System", user->getName() + " has joined the chat.");
    }

    void broadcast(const std::string& from, const std::string& message) {
        for (auto* user : users_) {
            if (user->getName() != from)
                user->receive(from, message);
        }
    }

    void sendPrivate(const std::string& from, const std::string& to,
                     const std::string& message) {
        for (auto* user : users_) {
            if (user->getName() == to) {
                user->receive(from, message);
                return;
            }
        }
        std::cout << "  [Error] User '" << to << "' not found.\n";
    }

private:
    std::vector<User*> users_;
};

void User::sendMessage(const std::string& message) const {
    std::cout << "  [" << name_ << " sends] " << message << "\n";
    room_->broadcast(name_, message);
}

void User::sendPrivateMessage(const std::string& to, const std::string& message) const {
    std::cout << "  [" << name_ << " -> " << to << " (private)] " << message << "\n";
    room_->sendPrivate(name_, to, message);
}

void User::receive(const std::string& from, const std::string& message) const {
    std::cout << "    " << name_ << " received from " << from << ": " << message << "\n";
}

int main() {
    std::cout << "\n";
    std::cout << "####################################################\n";
    std::cout << "#     Mediator Pattern - C++ Demonstration         #\n";
    std::cout << "####################################################\n\n";

    ChatRoom chatRoom;

    User alice("Alice");
    User bob("Bob");
    User charlie("Charlie");

    std::cout << "--- Users joining chat room ---\n\n";
    chatRoom.addUser(&alice);
    chatRoom.addUser(&bob);
    chatRoom.addUser(&charlie);

    std::cout << "\n--- Alice broadcasts a message ---\n\n";
    alice.sendMessage("Hello everyone!");

    std::cout << "\n--- Bob broadcasts a message ---\n\n";
    bob.sendMessage("Hi Alice! How's the project going?");

    std::cout << "\n--- Charlie sends private message to Alice ---\n\n";
    charlie.sendPrivateMessage("Alice", "Can you review my PR?");

    std::cout << "\n--- Alice replies privately to Charlie ---\n\n";
    alice.sendPrivateMessage("Charlie", "Sure, I'll look at it this afternoon.");

    std::cout << "\n";
    std::cout << "Key point: Users don't hold references to each other.\n";
    std::cout << "All communication goes through the ChatRoom mediator.\n";
    std::cout << "Adding/removing users or changing routing logic only\n";
    std::cout << "affects the mediator, not individual users.\n";

    return 0;
}
