#include "ReactiveGraph.hpp"

#include <algorithm>
#include <format>
#include <queue>
#include <ranges>
#include <stdexcept>

namespace nvidia::write_protect
{

SourceNodeId Graph::addSource(bool initial)
{
    std::size_t id = nodes.size();
    nodes.emplace_back(NodeType::Source, initial);
    return SourceNodeId{id};
}

NodeId Graph::addNode(BoolOp op)
{
    std::size_t id = nodes.size();
    auto type = (op == BoolOp::And) ? NodeType::And : NodeType::Or;
    nodes.emplace_back(type);
    return NodeId{id};
}

void Graph::connect(NodeId from, NodeId to)
{
    if (nodes[to.id].type == NodeType::Source)
    {
        throw std::runtime_error(
            std::format("Cannot connect to source node ({})", to.id));
    }
    if (from.id == to.id || isReachable(to.id, from.id))
    {
        throw std::runtime_error(std::format(
            "Connecting {} -> {} would create a cycle", from.id, to.id));
    }
    nodes[from.id].outgoing.push_back(to.id);
    nodes[to.id].incoming.push_back(from.id);
    touch(to.id);
}

void Graph::set(SourceNodeId id, bool value)
{
    touch(id.id);
    nodes[id.id].output = value;
}

void Graph::touch(SourceNodeId id)
{
    auto& node = nodes[id.id];
    if (!node.dirty)
    {
        node.dirty = true;
        node.snapshot = node.output;
        dirty.push_back(id.id);
    }
}

void Graph::propagate()
{
    if (dirty.empty())
    {
        return;
    }

    std::vector<bool> affected(nodes.size(), false);
    std::queue<std::size_t> queue;
    std::vector<std::size_t> changed;

    changed.reserve(dirty.size());

    for (std::size_t src : dirty)
    {
        if (!affected[src])
        {
            affected[src] = true;
            changed.push_back(src);
        }
        nodes[src].dirty = false;
        for (std::size_t next : nodes[src].outgoing)
        {
            queue.push(next);
        }
    }

    dirty.clear();

    while (!queue.empty())
    {
        std::size_t curr = queue.front();
        queue.pop();

        auto& node = nodes[curr];
        bool result = evaluate(node);

        if (result != node.output)
        {
            node.output = result;
            if (!affected[curr])
            {
                affected[curr] = true;
                changed.push_back(curr);
            }
            for (std::size_t next : node.outgoing)
            {
                queue.push(next);
            }
        }
    }

    for (std::size_t id : changed)
    {
        auto& node = nodes[id];
        if (node.output != node.snapshot)
        {
            for (const auto& cb : nodes[id].observers)
            {
                cb(nodes[id].output);
            }
        }
        node.snapshot = node.output;
    }
}

bool Graph::output(NodeId id) const
{
    const auto& node = nodes[id.id];
    if (node.type != NodeType::Source && node.incoming.size() == 0)
    {
        return false;
    }
    return node.output;
}

void Graph::subscribe(NodeId id, Callback cb)
{
    nodes[id.id].observers.push_back(std::move(cb));
}

void Graph::clearObservers(NodeId id)
{
    nodes[id.id].observers.clear();
}

void Graph::deactivate(NodeId id)
{
    auto& node = nodes[id.id];

    for (auto parentId : node.incoming)
    {
        std::erase(nodes[parentId].outgoing, id.id);
    }

    auto children = node.outgoing;
    for (auto childId : children)
    {
        std::erase(nodes[childId].incoming, id.id);
    }

    node = {};

    for (auto childId : children)
    {
        auto& child = nodes[childId];
        bool result = evaluate(child);
        if (result != child.output)
        {
            touch(childId);
            child.output = result;
        }
    }
}

bool Graph::evaluate(const Node& node) const
{
    auto val = [&](std::size_t id) { return nodes[id].output; };
    if (node.type == NodeType::And)
    {
        return node.incoming.size() > 0 &&
               std::ranges::all_of(node.incoming, val);
    }
    return std::ranges::any_of(node.incoming, val);
}

bool Graph::isReachable(std::size_t start, std::size_t target) const
{
    std::vector<bool> visited(nodes.size(), false);
    std::queue<std::size_t> queue;
    queue.push(start);
    visited[start] = true;

    while (!queue.empty())
    {
        std::size_t curr = queue.front();
        queue.pop();
        for (std::size_t next : nodes[curr].outgoing)
        {
            if (next == target)
            {
                return true;
            }
            if (!visited[next])
            {
                visited[next] = true;
                queue.push(next);
            }
        }
    }
    return false;
}

} // namespace nvidia::write_protect
