#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace nvidia::write_protect
{

class Graph;
class SourceNodeId;

/// @brief Opaque handle to any node in the graph.
class NodeId
{
    std::size_t id;

    constexpr NodeId(std::size_t id) : id(id) {}

    friend class SourceNodeId;
    friend class Graph;
    friend struct std::hash<NodeId>;

  public:
    constexpr NodeId() : id(SIZE_MAX) {}

    constexpr bool operator==(const NodeId& o) const
    {
        return id == o.id;
    }
    constexpr bool operator!=(const NodeId& o) const
    {
        return id != o.id;
    }
};

/// @brief Opaque handle to a source (input) node.  Implicitly converts
///        to @c NodeId for use with observer and connection APIs.
class SourceNodeId
{
    std::size_t id;

    constexpr SourceNodeId(std::size_t id) : id(id) {}

    friend class Graph;
    friend struct std::hash<SourceNodeId>;

  public:
    constexpr SourceNodeId() : id(SIZE_MAX) {}

    constexpr operator NodeId() const
    {
        return NodeId{id};
    }

    constexpr bool operator==(const SourceNodeId& o) const
    {
        return id == o.id;
    }
    constexpr bool operator!=(const SourceNodeId& o) const
    {
        return id != o.id;
    }
};

/// @brief Boolean operator for compute nodes.
enum class BoolOp
{
    And,
    Or
};

/**
 * @brief Reactive boolean directed acyclic graph.
 *
 * Source nodes hold externally-set boolean values.  Compute nodes combine
 * their inputs with a boolean operator.  Observers are callbacks that fire
 * when a node's output changes after propagation.  Cycle detection
 * prevents invalid topologies.
 *
 * All operations are synchronous and intended to run on a single-threaded
 * async context.
 */
class Graph
{
  public:
    using Callback = std::function<void(bool)>;

    /// @brief Create a source node with the given initial value.
    SourceNodeId addSource(bool initial = false);

    /// @brief Create a compute node with the given boolean operator.
    NodeId addNode(BoolOp op);

    /// @brief Add a directed edge.  Throws on cycle or if @p to is a source.
    void connect(NodeId from, NodeId to);

    /// @brief Set a source node's value and mark it dirty.
    void set(SourceNodeId id, bool value);

    /// @brief Mark a source node dirty without changing its value.
    void touch(SourceNodeId id);

    /// @brief BFS from dirty nodes, re-evaluate downstream, notify observers
    ///        whose output changed.
    void propagate();

    /// @brief Read a node's current output value.
    bool output(NodeId id) const;

    /// @brief Register an observer callback on a node.
    void subscribe(NodeId id, Callback cb);

    /// @brief Remove all observers from a node.
    void clearObservers(NodeId id);

    /// @brief Disconnect a node from all neighbors and clear its observers.
    void deactivate(NodeId id);

  private:
    enum class NodeType
    {
        Source,
        And,
        Or
    };

    struct Node
    {
        NodeType type;
        // False also for And nodes: evaluate() derives results solely from
        // incoming edges, and downstream reads of a no-input node must match
        // output()'s false masking.
        bool output = false;
        bool dirty = false;
        bool snapshot = output;
        std::vector<std::size_t> incoming;
        std::vector<std::size_t> outgoing;
        std::vector<Callback> observers;
    };

    bool evaluate(const Node& node) const;
    bool isReachable(std::size_t start, std::size_t target) const;

    std::vector<Node> nodes;
    std::vector<std::size_t> dirty;
};

} // namespace nvidia::write_protect

template <>
struct std::hash<nvidia::write_protect::NodeId>
{
    std::size_t operator()(nvidia::write_protect::NodeId n) const noexcept
    {
        return std::hash<std::size_t>{}(n.id);
    }
};

template <>
struct std::hash<nvidia::write_protect::SourceNodeId>
{
    std::size_t operator()(nvidia::write_protect::SourceNodeId n) const noexcept
    {
        return std::hash<std::size_t>{}(n.id);
    }
};
