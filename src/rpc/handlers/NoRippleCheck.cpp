#include <ripple/protocol/TxFlags.h>
#include <rpc/RPCHelpers.h>

namespace RPC {

boost::json::object
getBaseTx(
    ripple::AccountID const& accountID,
    std::uint32_t accountSeq,
    ripple::Fees const& fees)
{
    boost::json::object tx;
    tx["Sequence"] = accountSeq;
    tx["Account"] = ripple::toBase58(accountID);
    tx["Fee"] = RPC::toBoostJson(fees.units.jsonClipped());
    return tx;
}

Result
doNoRippleCheck(Context const& context)
{
    auto const& request = context.params;

    auto accountID =
        accountFromStringStrict(getRequiredString(request, "account"));

    if (!accountID)
        return Status{Error::rpcINVALID_PARAMS, "malformedAccount"};

    std::string role = getRequiredString(request, "role");
    bool roleGateway = false;
    {
        if (role == "gateway")
            roleGateway = true;
        else if (role != "user")
            return Status{Error::rpcINVALID_PARAMS, "role field is invalid"};
    }

    size_t limit = getUInt(request, "limit", 300);

    bool includeTxs = getBool(request, "transactions", false);

    auto v = ledgerInfoFromRequest(context);
    if (auto status = std::get_if<Status>(&v))
        return *status;

    auto lgrInfo = std::get<ripple::LedgerInfo>(v);
    std::optional<ripple::Fees> fees = includeTxs
        ? context.backend->fetchFees(lgrInfo.seq, context.yield)
        : std::nullopt;

    boost::json::array transactions;

    auto keylet = ripple::keylet::account(*accountID);
    auto accountObj = context.backend->fetchLedgerObject(
        keylet.key, lgrInfo.seq, context.yield);
    if (!accountObj)
        throw AccountNotFoundError(ripple::toBase58(*accountID));

    ripple::SerialIter it{accountObj->data(), accountObj->size()};
    ripple::SLE sle{it, keylet.key};

    std::uint32_t accountSeq = sle.getFieldU32(ripple::sfSequence);

    boost::json::array problems;
    bool bDefaultRipple =
        sle.getFieldU32(ripple::sfFlags) & ripple::lsfDefaultRipple;
    if (bDefaultRipple & !roleGateway)
    {
        problems.push_back(
            "You appear to have set your default ripple flag even though "
            "you "
            "are not a gateway. This is not recommended unless you are "
            "experimenting");
    }
    else if (roleGateway & !bDefaultRipple)
    {
        problems.push_back(
            "You should immediately set your default ripple flag");
        if (includeTxs)
        {
            auto tx = getBaseTx(*accountID, accountSeq++, *fees);
            tx["TransactionType"] = "AccountSet";
            tx["SetFlag"] = 8;
            transactions.push_back(tx);
        }
    }

    traverseOwnedNodes(
        *context.backend,
        *accountID,
        lgrInfo.seq,
        {},
        context.yield,
        [roleGateway,
         includeTxs,
         &fees,
         &transactions,
         &accountSeq,
         &limit,
         &accountID,
         &problems](auto const& ownedItem) {
            if (ownedItem.getType() == ripple::ltRIPPLE_STATE)
            {
                bool const bLow = accountID ==
                    ownedItem.getFieldAmount(ripple::sfLowLimit).getIssuer();

                bool const bNoRipple = ownedItem.getFieldU32(ripple::sfFlags) &
                    (bLow ? ripple::lsfLowNoRipple : ripple::lsfHighNoRipple);

                std::string problem;
                bool needFix = false;
                if (bNoRipple & roleGateway)
                {
                    problem = "You should clear the no ripple flag on your ";
                    needFix = true;
                }
                else if (!bNoRipple & !roleGateway)
                {
                    problem =
                        "You should probably set the no ripple flag on "
                        "your ";
                    needFix = true;
                }
                if (needFix)
                {
                    ripple::AccountID peer =
                        ownedItem
                            .getFieldAmount(
                                bLow ? ripple::sfHighLimit : ripple::sfLowLimit)
                            .getIssuer();
                    ripple::STAmount peerLimit = ownedItem.getFieldAmount(
                        bLow ? ripple::sfHighLimit : ripple::sfLowLimit);
                    problem += to_string(peerLimit.getCurrency());
                    problem += " line to ";
                    problem += to_string(peerLimit.getIssuer());
                    problems.emplace_back(problem);
                    if (includeTxs)
                    {
                        ripple::STAmount limitAmount(ownedItem.getFieldAmount(
                            bLow ? ripple::sfLowLimit : ripple::sfHighLimit));
                        limitAmount.setIssuer(peer);
                        auto tx = getBaseTx(*accountID, accountSeq++, *fees);
                        tx["TransactionType"] = "TrustSet";
                        tx["LimitAmount"] = RPC::toBoostJson(
                            limitAmount.getJson(ripple::JsonOptions::none));
                        tx["Flags"] = bNoRipple ? ripple::tfClearNoRipple
                                                : ripple::tfSetNoRipple;
                        transactions.push_back(tx);
                    }

                    if (limit-- == 0)
                        return false;
                }
            }
            return true;
        });

    boost::json::object response;
    response["ledger_index"] = lgrInfo.seq;
    response["ledger_hash"] = ripple::strHex(lgrInfo.hash);
    response["problems"] = std::move(problems);
    if (includeTxs)
        response["transactions"] = std::move(transactions);

    return response;
}

}  // namespace RPC
