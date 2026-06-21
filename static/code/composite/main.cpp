/**
 * ============================================================================
 * 组合模式 (Composite Pattern) - 结构型设计模式
 * ============================================================================
 *
 * 【解决的问题】
 *
 * 当系统中存在"部分-整体"的树形层次结构，且客户端需要以统一的方式
 * 处理单个对象（叶子）和组合对象（容器）时，如果用 if/else 区分处理，
 * 代码会变得极其复杂且难以扩展。
 *
 * 组合模式通过让叶子节点和组合节点实现同一个接口（Component），使得
 * 客户端可以用一致的方式操作单个对象和组合对象，无需关心当前处理的
 * 是叶子还是容器。
 *
 * 【核心思想】
 *
 *   - 定义统一的 Component 接口
 *   - Leaf（叶子）：没有子节点的对象，如文件
 *   - Composite（组合）：包含子节点的对象，如文件夹
 *   - 客户端只依赖 Component 接口，递归操作整棵树
 *
 * 【典型场景】
 *   - 文件系统（文件 + 文件夹）
 *   - GUI 组件树（按钮 + 面板 + 窗口）
 *   - 组织架构（员工 + 部门）
 *   - 表达式树
 *
 * 【与桥接模式、中介者模式的区别】
 *
 *   组合模式 (Composite):
 *     - 属于结构型模式
 *     - 解决「树形结构的统一操作」问题
 *     - 关注对象的"组成关系"：整体由部分构成，部分又可以包含更小的部分
 *     - 核心技巧：叶子和容器共享同一接口，递归组合
 *
 *   桥接模式 (Bridge):
 *     - 属于结构型模式
 *     - 解决「多维度独立变化」的耦合问题
 *     - 关注对象的"使用关系"：把抽象和实现分离，让它们可以独立变化
 *     - 核心技巧：用组合替代继承，将继承关系拆成两个独立的继承体系
 *
 *   中介者模式 (Mediator):
 *     - 属于行为型模式
 *     - 解决「多对象之间网状通信」的复杂性问题
 *     - 关注对象的"通信关系"：把多对多通信变为一对多（通过中介）
 *     - 核心技巧：引入中介者对象集中管理交互逻辑
 *
 * 【一句话总结】
 *   组合 → 统一处理树形结构中的叶子和容器
 *   桥接 → 拆分多维度变化，让抽象和实现独立演进
 *   中介 → 收拢多对象通信，消灭网状依赖
 *
 * ============================================================================
 */

#include <iostream>
#include <string>
#include <algorithm>
#include <vector>
#include <memory>

// ============================================================================
// Component: 统一接口，叶子和容器都实现它
// ============================================================================

class FileSystemNode {
public:
    virtual ~FileSystemNode() = default;

    virtual std::string getName() const = 0;
    virtual size_t getSize() const = 0;

    virtual void addChild(std::shared_ptr<FileSystemNode> child) {
        throw std::runtime_error("Leaf node cannot have children");
    }

    virtual void removeChild(const std::string& name) {
        throw std::runtime_error("Leaf node cannot remove children");
    }

    virtual void print(const std::string& indent = "") const = 0;
};

// ============================================================================
// Leaf: 文件（叶子节点）
// ============================================================================

class File : public FileSystemNode {
public:
    File(std::string name, size_t size)
        : name_(std::move(name)), size_(size) {}

    std::string getName() const override { return name_; }
    size_t getSize() const override { return size_; }

    void print(const std::string& indent = "") const override {
        std::cout << indent << "- " << name_ << " (" << size_ << " KB)\n";
    }

private:
    std::string name_;
    size_t size_;
};

// ============================================================================
// Composite: 文件夹（组合节点）
// ============================================================================

class Directory : public FileSystemNode {
public:
    explicit Directory(std::string name) : name_(std::move(name)) {}

    std::string getName() const override { return name_; }

    size_t getSize() const override {
        size_t total = 0;
        for (const auto& child : children_) {
            total += child->getSize();
        }
        return total;
    }

    void addChild(std::shared_ptr<FileSystemNode> child) override {
        children_.push_back(std::move(child));
    }

    void removeChild(const std::string& name) override {
        children_.erase(
            std::remove_if(children_.begin(), children_.end(),
                [&name](const auto& c) { return c->getName() == name; }),
            children_.end()
        );
    }

    void print(const std::string& indent = "") const override {
        std::cout << indent << "+ " << name_ << "/ (" << getSize() << " KB total)\n";
        for (const auto& child : children_) {
            child->print(indent + "  ");
        }
    }

private:
    std::string name_;
    std::vector<std::shared_ptr<FileSystemNode>> children_;
};

// ============================================================================
// Main: 演示统一操作文件系统的树形结构
// ============================================================================

int main() {
    std::cout << "\n";
    std::cout << "####################################################\n";
    std::cout << "#     Composite Pattern - C++ Demonstration        #\n";
    std::cout << "####################################################\n\n";

    auto root = std::make_shared<Directory>("project");

    auto srcDir = std::make_shared<Directory>("src");
    srcDir->addChild(std::make_shared<File>("main.cpp", 12));
    srcDir->addChild(std::make_shared<File>("utils.cpp", 8));
    srcDir->addChild(std::make_shared<File>("utils.h", 3));

    auto docDir = std::make_shared<Directory>("docs");
    docDir->addChild(std::make_shared<File>("README.md", 5));
    docDir->addChild(std::make_shared<File>("design.md", 15));

    auto assetsDir = std::make_shared<Directory>("assets");
    auto imgDir = std::make_shared<Directory>("images");
    imgDir->addChild(std::make_shared<File>("logo.png", 200));
    imgDir->addChild(std::make_shared<File>("banner.jpg", 350));
    assetsDir->addChild(imgDir);
    assetsDir->addChild(std::make_shared<File>("style.css", 7));

    root->addChild(srcDir);
    root->addChild(docDir);
    root->addChild(assetsDir);
    root->addChild(std::make_shared<File>("Makefile", 2));

    std::cout << "File system tree:\n\n";
    root->print("  ");

    std::cout << "\n";
    std::cout << "Key point: client code treats File and Directory\n";
    std::cout << "identically through the FileSystemNode interface.\n";
    std::cout << "No if/else needed to check node type.\n";

    return 0;
}
