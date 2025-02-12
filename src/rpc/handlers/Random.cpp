#include <ripple/beast/utility/rngfill.h>
#include <ripple/crypto/csprng.h>
#include <rpc/RPCHelpers.h>
namespace RPC {

Result
doRandom(Context const& context)
{
    ripple::uint256 rand;

    beast::rngfill(rand.begin(), rand.size(), ripple::crypto_prng());
    boost::json::object result;
    result["random"] = ripple::strHex(rand);
    return result;
}
}  // namespace RPC
