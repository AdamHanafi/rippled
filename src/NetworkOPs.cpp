
#include "NetworkOPs.h"

#include <boost/bind.hpp>
#include <boost/unordered_map.hpp>

#include "utils.h"
#include "Application.h"
#include "Transaction.h"
#include "LedgerConsensus.h"
#include "LedgerTiming.h"
#include "Log.h"

// This is the primary interface into the "client" portion of the program.
// Code that wants to do normal operations on the network such as
// creating and monitoring accounts, creating transactions, and so on
// should use this interface. The RPC code will primarily be a light wrapper
// over this code.

// Eventually, it will check the node's operating mode (synched, unsynched,
// etectera) and defer to the correct means of processing. The current
// code assumes this node is synched (and will continue to do so until
// there's a functional network.

NetworkOPs::NetworkOPs(boost::asio::io_service& io_service, LedgerMaster* pLedgerMaster) :
	mMode(omDISCONNECTED),mNetTimer(io_service), mLedgerMaster(pLedgerMaster)
{
}

boost::posix_time::ptime NetworkOPs::getNetworkTimePT()
{
	return boost::posix_time::second_clock::universal_time();
}

uint64 NetworkOPs::getNetworkTimeNC()
{
	return iToSeconds(getNetworkTimePT());
}

uint32 NetworkOPs::getCurrentLedgerID()
{
	return mLedgerMaster->getCurrentLedger()->getLedgerSeq();
}

Transaction::pointer NetworkOPs::processTransaction(Transaction::pointer trans, uint32 tgtLedger, Peer* source)
{
	Transaction::pointer dbtx = theApp->getMasterTransaction().fetch(trans->getID(), true);
	if (dbtx) return dbtx;

	if (!trans->checkSign())
	{
		Log(lsINFO) << "Transaction has bad signature";
		trans->setStatus(INVALID);
		return trans;
	}

	TransactionEngineResult r = mLedgerMaster->doTransaction(*trans->getSTransaction(), tgtLedger, tepNONE);
	if (r == tenFAILED) throw Fault(IO_ERROR);

	if (r == terPRE_SEQ)
	{ // transaction should be held
		Log(lsDEBUG) << "Transaction should be held";
		trans->setStatus(HELD);
		theApp->getMasterTransaction().canonicalize(trans, true);
		mLedgerMaster->addHeldTransaction(trans);
		return trans;
	}
	if ((r == terPAST_SEQ) || (r == terPAST_LEDGER))
	{ // duplicate or conflict
		Log(lsINFO) << "Transaction is obsolete";
		trans->setStatus(OBSOLETE);
		return trans;
	}

	if (r == terSUCCESS)
	{
		Log(lsINFO) << "Transaction is now included";
		trans->setStatus(INCLUDED);
		theApp->getMasterTransaction().canonicalize(trans, true);

// FIXME: Need code to get all accounts affected by a transaction and re-synch
// any of them that affect local accounts cached in memory. Or, we need to
// no cache the account balance information and always get it from the current ledger
//		theApp->getWallet().applyTransaction(trans);

		newcoin::TMTransaction tx;
		Serializer s;
		trans->getSTransaction()->add(s);
		tx.set_rawtransaction(&s.getData().front(), s.getLength());
		tx.set_status(newcoin::tsCURRENT);
		tx.set_receivetimestamp(getNetworkTimeNC());
		tx.set_ledgerindexpossible(trans->getLedger());

		PackedMessage::pointer packet = boost::make_shared<PackedMessage>(tx, newcoin::mtTRANSACTION);
		theApp->getConnectionPool().relayMessage(source, packet);

		return trans;
	}

	Log(lsDEBUG) << "Status other than success " << r ;
	if ((mMode != omFULL) && (theApp->suppress(trans->getID())))
	{
		newcoin::TMTransaction tx;
		Serializer s;
		trans->getSTransaction()->add(s);
		tx.set_rawtransaction(&s.getData().front(), s.getLength());
		tx.set_status(newcoin::tsCURRENT);
		tx.set_receivetimestamp(getNetworkTimeNC());
		tx.set_ledgerindexpossible(tgtLedger);
		PackedMessage::pointer packet = boost::make_shared<PackedMessage>(tx, newcoin::mtTRANSACTION);
		theApp->getConnectionPool().relayMessage(source, packet);
	}

	trans->setStatus(INVALID);
	return trans;
}

Transaction::pointer NetworkOPs::findTransactionByID(const uint256& transactionID)
{
	return Transaction::load(transactionID);
}

int NetworkOPs::findTransactionsBySource(const uint256& uLedger, std::list<Transaction::pointer>& txns,
	const NewcoinAddress& sourceAccount, uint32 minSeq, uint32 maxSeq)
{
	AccountState::pointer state = getAccountState(uLedger, sourceAccount);
	if (!state) return 0;
	if (minSeq > state->getSeq()) return 0;
	if (maxSeq > state->getSeq()) maxSeq = state->getSeq();
	if (maxSeq > minSeq) return 0;

	int count = 0;
	for(int i = minSeq; i <= maxSeq; ++i)
	{
		Transaction::pointer txn = Transaction::findFrom(sourceAccount, i);
		if(txn)
		{
			txns.push_back(txn);
			++count;
		}
	}
	return count;
}

int NetworkOPs::findTransactionsByDestination(std::list<Transaction::pointer>& txns,
	const NewcoinAddress& destinationAccount, uint32 startLedgerSeq, uint32 endLedgerSeq, int maxTransactions)
{
	// WRITEME
	return 0;
}

//
// Account functions
//

AccountState::pointer NetworkOPs::getAccountState(const uint256& uLedger, const NewcoinAddress& accountID)
{
	return mLedgerMaster->getLedgerByHash(uLedger)->getAccountState(accountID);
}

SLE::pointer NetworkOPs::getGenerator(const uint256& uLedger, const uint160& uGeneratorID)
{
	LedgerStateParms	qry				= lepNONE;

	return mLedgerMaster->getLedgerByHash(uLedger)->getGenerator(qry, uGeneratorID);
}

//
// Directory functions
//

// <-- false : no entrieS
bool NetworkOPs::getDirInfo(
	const uint256&			uLedger,
	const uint256&			uBase,
	uint256&				uDirLineNodeFirst,
	uint256&				uDirLineNodeLast)
{
	uint256				uRootIndex	= Ledger::getDirIndex(uBase, 0);
	LedgerStateParms	lspRoot		= lepNONE;
	SLE::pointer		sleRoot		= mLedgerMaster->getLedgerByHash(uLedger)->getDirRoot(lspRoot, uRootIndex);

	if (sleRoot)
	{
		Log(lsDEBUG) << "getDirInfo: root index: " << uRootIndex.ToString() ;

		Log(lsTRACE) << "getDirInfo: first: " << strHex(sleRoot->getIFieldU64(sfFirstNode)) ;
		Log(lsTRACE) << "getDirInfo:  last: " << strHex(sleRoot->getIFieldU64(sfLastNode)) ;

		uDirLineNodeFirst	= Ledger::getDirIndex(uBase, sleRoot->getIFieldU64(sfFirstNode));
		uDirLineNodeLast	= Ledger::getDirIndex(uBase, sleRoot->getIFieldU64(sfLastNode));

		Log(lsTRACE) << "getDirInfo: first: " << uDirLineNodeFirst.ToString() ;
		Log(lsTRACE) << "getDirInfo:  last: " << uDirLineNodeLast.ToString() ;
	}
	else
	{
		Log(lsINFO) << "getDirInfo: root index: NOT FOUND: " << uRootIndex.ToString() ;
	}

	return !!sleRoot;
}

STVector256 NetworkOPs::getDirNode(const uint256& uLedger, const uint256& uDirLineNode)
{
	STVector256	svIndexes;

	LedgerStateParms	lspNode		= lepNONE;
	SLE::pointer		sleNode		= mLedgerMaster->getLedgerByHash(uLedger)->getDirNode(lspNode, uDirLineNode);

	if (sleNode)
	{
		Log(lsWARNING) << "getDirNode: node index: " << uDirLineNode.ToString() ;

		svIndexes	= sleNode->getIFieldV256(sfIndexes);
	}
	else
	{
		Log(lsINFO) << "getDirNode: node index: NOT FOUND: " << uDirLineNode.ToString() ;
	}

	return svIndexes;
}

//
// Nickname functions
//

NicknameState::pointer NetworkOPs::getNicknameState(const uint256& uLedger, const std::string& strNickname)
{
	return mLedgerMaster->getLedgerByHash(uLedger)->getNicknameState(strNickname);
}

//
// Ripple functions
//

RippleState::pointer NetworkOPs::getRippleState(const uint256& uLedger, const uint256& uIndex)
{
	return mLedgerMaster->getLedgerByHash(uLedger)->getRippleState(uIndex);
}

//
// Other
//

void NetworkOPs::setStateTimer(int sec)
{ // set timer early if ledger is closing
	uint64 consensusTime = mLedgerMaster->getCurrentLedger()->getCloseTimeNC() - LEDGER_WOBBLE_TIME;
	uint64 now = getNetworkTimeNC();

	if ((mMode == omFULL) && !mConsensus)
	{
		if (now >= consensusTime) sec = 0;
		else if (sec > (consensusTime - now)) sec = (consensusTime - now);
	}

	mNetTimer.expires_from_now(boost::posix_time::seconds(sec));
	mNetTimer.async_wait(boost::bind(&NetworkOPs::checkState, this, boost::asio::placeholders::error));
}

class ValidationCount
{
public:
	int trustedValidations, untrustedValidations, nodesUsing;
	NewcoinAddress highNode;

	ValidationCount() : trustedValidations(0), untrustedValidations(0), nodesUsing(0) { ; }
	bool operator>(const ValidationCount& v)
	{
		if (trustedValidations > v.trustedValidations) return true;
		if (trustedValidations < v.trustedValidations) return false;
		if (untrustedValidations > v.untrustedValidations) return true;
		if (untrustedValidations < v.untrustedValidations) return false;
		if (nodesUsing > v.nodesUsing) return true;
		if (nodesUsing < v.nodesUsing) return false;
		return highNode > v.highNode;
	}
};

void NetworkOPs::checkState(const boost::system::error_code& result)
{ // Network state machine
	if (result == boost::asio::error::operation_aborted)
		return;

	std::vector<Peer::pointer> peerList = theApp->getConnectionPool().getPeerVector();

	// do we have sufficient peers? If not, we are disconnected.
	if (peerList.size() < theConfig.NETWORK_QUORUM)
	{
		if (mMode != omDISCONNECTED)
		{
			setMode(omDISCONNECTED);
			Log(lsWARNING) << "Node count (" << peerList.size() <<
				") has fallen below quorum (" << theConfig.NETWORK_QUORUM << ").";
		}
		setStateTimer(5);
		return;
	}
	if (mMode == omDISCONNECTED)
	{
		setMode(omCONNECTED);
		Log(lsINFO) << "Node count (" << peerList.size() << ") is sufficient.";
	}

	if (mConsensus)
	{
		setStateTimer(mConsensus->timerEntry());
		return;
	}

	// Do we have sufficient validations for our last closed ledger? Or do sufficient nodes
	// agree? And do we have no better ledger available?
	// If so, we are either tracking or full.
	boost::unordered_map<uint256, ValidationCount> ledgers;

	for (std::vector<Peer::pointer>::iterator it = peerList.begin(), end = peerList.end(); it != end; ++it)
	{
		if (!*it)
		{
			Log(lsDEBUG) << "NOP::CS Dead pointer in peer list";
		}
		else
		{
			uint256 peerLedger = (*it)->getClosedLedgerHash();
			if (!!peerLedger)
			{
				// FIXME: If we have this ledger, don't count it if it's too far past its close time
				bool isNew = ledgers.find(peerLedger) == ledgers.end();
				ValidationCount& vc = ledgers[peerLedger];
				if (isNew)
				{
					theApp->getValidations().getValidationCount(peerLedger,
						vc.trustedValidations, vc.untrustedValidations);
					Log(lsTRACE) << peerLedger.GetHex() << " has " << vc.trustedValidations <<
						" trusted validations and " << vc.untrustedValidations << " untrusted";
				}
				if ((vc.nodesUsing == 0) || ((*it)->getNodePublic() > vc.highNode))
					vc.highNode = (*it)->getNodePublic();
				++vc.nodesUsing;
			}
		}
	}

	Ledger::pointer currentClosed = mLedgerMaster->getClosedLedger();
	uint256 closedLedger = currentClosed->getHash();
	ValidationCount& vc = ledgers[closedLedger];
	if ((vc.nodesUsing == 0) || (theApp->getWallet().getNodePublic() > vc.highNode))
		vc.highNode = theApp->getWallet().getNodePublic();
	++ledgers[closedLedger].nodesUsing;

	// 3) Is there a network ledger we'd like to switch to? If so, do we have it?
	bool switchLedgers = false;
	for(boost::unordered_map<uint256, ValidationCount>::iterator it = ledgers.begin(), end = ledgers.end();
		it != end; ++it)
	{
		if (it->second > vc)
		{
			vc = it->second;
			closedLedger = it->first;
			switchLedgers = true;
		}
	}


	if (switchLedgers)
	{
		Log(lsWARNING) << "We are not running on the consensus ledger";
		Log(lsINFO) << "Our LCL " << currentClosed->getHash().GetHex() ;
		Log(lsINFO) << "Net LCL " << closedLedger.GetHex() ;
		if ((mMode == omTRACKING) || (mMode == omFULL)) setMode(omTRACKING);
		Ledger::pointer consensus = mLedgerMaster->getLedgerByHash(closedLedger);
		if (!consensus)
		{
			Log(lsINFO) << "Acquiring consensus ledger";
			LedgerAcquire::pointer acq = theApp->getMasterLedgerAcquire().findCreate(closedLedger);
			if (!acq || acq->isFailed())
			{
				theApp->getMasterLedgerAcquire().dropLedger(closedLedger);
				Log(lsERROR) << "Network ledger cannot be acquired";
				setStateTimer(10);
				return;
			}
			if (!acq->isComplete())
			{ // add more peers
				// FIXME: A peer may not have a ledger just because it accepts it as the network's consensus
				for (std::vector<Peer::pointer>::iterator it = peerList.begin(), end = peerList.end(); it != end; ++it)
					if ((*it)->getClosedLedgerHash() == closedLedger)
						acq->peerHas(*it);
				setStateTimer(5);
				return;
			}
			consensus = acq->getLedger();
		}
		switchLastClosedLedger(consensus);
	}

	// WRITEME: Unless we are in omFULL and in the process of doing a consensus,
	// we must count how many nodes share our LCL, how many nodes disagree with our LCL,
	// and how many validations our LCL has. We also want to check timing to make sure
	// there shouldn't be a newer LCL. We need this information to do the next three
	// tests.

	if (mMode == omCONNECTED)
	{ // count number of peers that agree with us and UNL nodes whose validations we have for LCL
		// if the ledger is good enough, go to omTRACKING - TODO
		if (!switchLedgers) setMode(omTRACKING);
	}

	if (mMode == omTRACKING)
	{
		// check if the ledger is good enough to go to omFULL
		// Note: Do not go to omFULL if we don't have the previous ledger
		// check if the ledger is bad enough to go to omCONNECTED -- TODO
		if ((!switchLedgers) && theConfig.VALIDATION_SEED.isValid()) setMode(omFULL);
	}

	if (mMode == omFULL)
	{
		// check if the ledger is bad enough to go to omTRACKING
	}

	int secondsToClose = theApp->getMasterLedger().getCurrentLedger()->getCloseTimeNC() -
		theApp->getOPs().getNetworkTimeNC();
	if ((!mConsensus) && (secondsToClose < LEDGER_WOBBLE_TIME)) // pre close wobble
		beginConsensus(theApp->getMasterLedger().getCurrentLedger());
	if (mConsensus)
		setStateTimer(mConsensus->timerEntry());
	else setStateTimer(10);
}

void NetworkOPs::switchLastClosedLedger(Ledger::pointer newLedger)
{ // set the newledger as our last closed ledger -- this is abnormal code

	Log(lsERROR) << "ABNORMAL Switching last closed ledger to " << newLedger->getHash().GetHex() ;

	if (mConsensus)
	{
		mConsensus->abort();
		mConsensus = boost::shared_ptr<LedgerConsensus>();
	}

	newLedger->setClosed();
	Ledger::pointer openLedger = boost::make_shared<Ledger>(false, boost::ref(*newLedger));
	mLedgerMaster->switchLedgers(newLedger, openLedger);

	if (getNetworkTimeNC() > openLedger->getCloseTimeNC())
	{ // this ledger has already closed
	}

}
// vim:ts=4

int NetworkOPs::beginConsensus(Ledger::pointer closingLedger)
{
	Log(lsINFO) << "Ledger close time for ledger " << closingLedger->getLedgerSeq() ;
	Log(lsINFO) << " LCL is " << closingLedger->getParentHash().GetHex();

	Ledger::pointer prevLedger = mLedgerMaster->getLedgerByHash(closingLedger->getParentHash());
	if (!prevLedger)
	{ // this shouldn't happen unless we jump ledgers
		Log(lsWARNING) << "Don't have LCL, going to tracking";
		setMode(omTRACKING);
		return 3;
	}
	assert(prevLedger->getHash() == closingLedger->getParentHash());
	assert(closingLedger->getParentHash() == mLedgerMaster->getClosedLedger()->getHash());

	// Create a consensus object to get consensus on this ledger
	if (!!mConsensus) mConsensus->abort();
	prevLedger->setImmutable();
	mConsensus = boost::make_shared<LedgerConsensus>
		(prevLedger, theApp->getMasterLedger().getCurrentLedger()->getCloseTimeNC());

	Log(lsDEBUG) << "Pre-close time, initiating consensus engine";
	return mConsensus->startup();
}

// <-- bool: true to relay
bool NetworkOPs::recvPropose(uint32 proposeSeq, const uint256& proposeHash,
	const std::string& pubKey, const std::string& signature)
{
	// XXX Validate key.
	// XXX Take a vuc for pubkey.
	NewcoinAddress	naPeerPublic	= NewcoinAddress::createNodePublic(strCopy(pubKey));

	if (mMode != omFULL)
	{
		Log(lsINFO) << "Received proposal when not full: " << mMode;
		Serializer s(signature);
		return theApp->suppress(s.getSHA512Half());
	}
	if (!mConsensus)
	{
		Log(lsWARNING) << "Received proposal when full but not during consensus window";
		return false;
	}

	boost::shared_ptr<LedgerConsensus> consensus = mConsensus;
	if (!consensus) return false;

	LedgerProposal::pointer proposal =
		boost::make_shared<LedgerProposal>(consensus->getLCL(), proposeSeq, proposeHash, naPeerPublic);
	if (!proposal->checkSign(signature))
	{ // Note that if the LCL is different, the signature check will fail
		Log(lsWARNING) << "Ledger proposal fails signature check";
		return false;
	}

	// Is this node on our UNL?
	// XXX Is this right?
	if (!theApp->getUNL().nodeInUNL(naPeerPublic))
	{
		Log(lsINFO) << "Relay, but no process peer proposal " << proposal->getProposeSeq() << "/"
			<< proposal->getCurrentHash().GetHex();
		return true;
	}

	return consensus->peerPosition(proposal);
}

SHAMap::pointer NetworkOPs::getTXMap(const uint256& hash)
{
	if (!mConsensus) return SHAMap::pointer();
	return mConsensus->getTransactionTree(hash, false);
}

bool NetworkOPs::gotTXData(boost::shared_ptr<Peer> peer, const uint256& hash,
	const std::list<SHAMapNode>& nodeIDs, const std::list< std::vector<unsigned char> >& nodeData)
{
	if (!mConsensus) return false;
	return mConsensus->peerGaveNodes(peer, hash, nodeIDs, nodeData);
}

bool NetworkOPs::hasTXSet(boost::shared_ptr<Peer> peer, const uint256& set, newcoin::TxSetStatus status)
{
	if (!mConsensus) return false;
	return mConsensus->peerHasSet(peer, set, status);
}

void NetworkOPs::mapComplete(const uint256& hash, SHAMap::pointer map)
{
	if (mConsensus)
		mConsensus->mapComplete(hash, map, true);
}

void NetworkOPs::endConsensus()
{
	mConsensus = boost::shared_ptr<LedgerConsensus>();
}

void NetworkOPs::setMode(OperatingMode om)
{
	if (mMode == om) return;
	if (mMode == omFULL)
	{
		if (mConsensus)
		{
			mConsensus->abort();
			mConsensus = boost::shared_ptr<LedgerConsensus>();
		}
	}
	Log l((om < mMode) ? lsWARNING : lsINFO);
	if (om == omDISCONNECTED) l << "STATE->Disonnected";
	else if (om == omCONNECTED) l << "STATE->Connected";
	else if (om == omTRACKING) l << "STATE->Tracking";
	else l << "STATE->Full";
	mMode = om;
}

std::vector< std::pair<uint32, uint256> >
	NetworkOPs::getAffectedAccounts(const NewcoinAddress& account, uint32 minLedger, uint32 maxLedger)
{
	std::vector< std::pair<uint32, uint256> > affectedAccounts;

	std::string sql =
		str(boost::format("SELECT LedgerSeq,TransID FROM AccountTransactions INDEXED BY AcctTxIndex "
			" WHERE Account = '%s' AND LedgerSeq <= '%d' AND LedgerSeq >= '%d' ORDER BY LedgerSeq LIMIT 1000")
			% account.humanAccountID() % maxLedger	% minLedger);

	Database *db = theApp->getTxnDB()->getDB();
	ScopedLock dbLock = theApp->getTxnDB()->getDBLock();

	SQL_FOREACH(db, sql)
	{
		std::string txID;
		db->getStr("TransID", txID);
		affectedAccounts.push_back(std::make_pair<uint32, uint256>(db->getInt("LedgerSeq"), uint256(txID)));
	}

	return affectedAccounts;
}

bool NetworkOPs::recvValidation(SerializedValidation::pointer val)
{
	return theApp->getValidations().addValidation(val);
}

// vim:ts=4
