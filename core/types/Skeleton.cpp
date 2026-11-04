#include "core/types/Skeleton.h"
#include <QVector>

namespace core {

QVector<int> Skeleton::sortParentsFirst()
{
    const int n = joints.size();
    if (n == 0 || parentsPrecedeChildren()) return {};   // nothing to do, and nothing to remap

    QVector<int> order;  order.reserve(n);
    QVector<char> state(n, 0);                           // 0 = unvisited, 1 = on stack, 2 = emitted
    QVector<int>  stack;

    // Iterative DFS: a rig deep enough to blow a recursive stack is exactly the malformed input
    // this has to survive.
    for (int root = 0; root < n; ++root) {
        if (state[root] == 2) continue;
        stack.clear();
        int cur = root;
        while (true) {
            if (cur < 0 || cur >= n) break;
            if (state[cur] == 2) break;
            if (state[cur] == 1) return {};              // cycle — refuse to reorder
            state[cur] = 1;
            stack.append(cur);
            const int p = joints[cur].parent;
            if (p < 0 || p >= n || state[p] == 2) break;
            cur = p;
        }
        for (int i = stack.size() - 1; i >= 0; --i) { state[stack[i]] = 2; order.append(stack[i]); }
    }
    if (order.size() != n) return {};                    // unreachable joints — refuse

    QVector<int> oldToNew(n, -1);
    for (int newIdx = 0; newIdx < n; ++newIdx) oldToNew[order[newIdx]] = newIdx;

    QVector<Joint> out;  out.reserve(n);
    for (int newIdx = 0; newIdx < n; ++newIdx) {
        Joint j = joints[order[newIdx]];
        j.parent = (j.parent >= 0) ? oldToNew[j.parent] : -1;
        out.append(j);
    }
    joints = out;
    return oldToNew;
}

}  // namespace core
