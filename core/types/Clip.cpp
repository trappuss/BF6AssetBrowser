#include "core/types/Clip.h"
#include <cmath>

namespace core {

bool Track::tIsConstant(const float restT[3], float eps) const
{
    for (const auto& v : t) for (int i = 0; i < 3; ++i) if (std::fabs(v[i] - restT[i]) > eps) return false;
    return true;
}

bool Track::sIsConstant(const float restS[3], float eps) const
{
    for (const auto& v : s) for (int i = 0; i < 3; ++i) if (std::fabs(v[i] - restS[i]) > eps) return false;
    return true;
}

bool Track::qIsConstant(const rm::Quat& restQ, float eps) const
{
    for (const auto& v : q) {
        // A quaternion and its negation are the same rotation, so compare both signs. A solver that
        // flips the sign mid-clip would otherwise look like it "varies".
        float d = 0; for (int i = 0; i < 4; ++i) d += v[i] * restQ[i];
        const float sgn = d < 0 ? -1.0f : 1.0f;
        for (int i = 0; i < 4; ++i) if (std::fabs(sgn * v[i] - restQ[i]) > eps) return false;
    }
    return true;
}

}  // namespace core
