//===- ThreadCallGraph.cpp -- Call graph considering thread fork/join---------//
//
//                     SVF: Static Value-Flow Analysis
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

/*
 * ThreadCallGraph.cpp
 *
 *  Created on: Jul 12, 2014
 *      Author: Yulei Sui, Peng Di
 */

#include "Util/ThreadCallGraph.h"
#include <llvm/IR/Module.h>
#include <llvm/IR/InstIterator.h>	// for inst iteration

using namespace llvm;
using namespace analysisUtil;

/*!
 * Constructor
 */
ThreadCallGraph::ThreadCallGraph(llvm::Module* module) :
    PTACallGraph(module), tdAPI(ThreadAPI::getThreadAPI()) {
    DBOUT(DGENERAL, llvm::outs() << analysisUtil::pasMsg("Building ThreadCallGraph\n"));
    this->build(module);
}

/*!
 * Start building Thread Call Graph
 */
void ThreadCallGraph::build(Module* m) {
    // create thread fork edges and record fork sites
    for (Module::const_iterator fi = m->begin(), efi = m->end(); fi != efi; ++fi) {
        for (const_inst_iterator II = inst_begin(*fi), E = inst_end(*fi); II != E; ++II) {
            const Instruction *inst = &*II;
            if (tdAPI->isTDFork(inst)) {
                addForksite(cast<CallInst>(inst));
                const Function* forkee = dyn_cast<Function>(tdAPI->getForkedFun(inst));
                if (forkee) {
                    addDirectForkEdge(inst);
                }
                // indirect call to the start routine function
                else {
                    addThreadForkEdgeSetMap(inst,NULL);
                }
            }
        }
    }
    // record join sites
    for (Module::const_iterator fi = m->begin(), efi = m->end(); fi != efi; ++fi) {
        for (const_inst_iterator II = inst_begin(*fi), E = inst_end(*fi); II != E; ++II) {
            const Instruction *inst = &*II;
            if (tdAPI->isTDJoin(inst)) {
                addJoinsite(cast<CallInst>(inst));
            }
        }
    }
}

/*
 * Update call graph using pointer analysis results
 * (1) resolve function pointers for non-fork calls
 * (2) resolve function pointers for fork sites
 */
void ThreadCallGraph::updateCallGraph(PointerAnalysis* pta) {

    PointerAnalysis::CallEdgeMap::const_iterator iter = pta->getIndCallMap().begin();
    PointerAnalysis::CallEdgeMap::const_iterator eiter = pta->getIndCallMap().end();
    for (; iter != eiter; iter++) {
        llvm::CallSite cs = iter->first;
        const Instruction *callInst = cs.getInstruction();
        const PTACallGraph::FunctionSet &functions = iter->second;
        for (PTACallGraph::FunctionSet::const_iterator func_iter =
                    functions.begin(); func_iter != functions.end(); func_iter++) {
            const Function *callee = *func_iter;
            this->addIndirectCallGraphEdge(callInst, callee);
        }
    }

    for (CallSiteSet::iterator it = forksitesBegin(), eit = forksitesEnd(); it != eit; ++it) {
        const Value* forkedval=tdAPI->getForkedFun(*it);
        if(dyn_cast<Function>(forkedval)==NULL) {
            PAG* pag = pta->getPAG();
            const PointsTo& targets = pta->getPts(pag->getValueNode(forkedval));
            for (PointsTo::iterator ii = targets.begin(), ie = targets.end(); ii != ie; ii++) {
                if(ObjPN* objPN = dyn_cast<ObjPN>(pag->getPAGNode(*ii))) {
                    const MemObj* obj = pag->getObject(objPN);
                    if(obj->isFunction()) {
                        const Function* callee = cast<Function>(obj->getRefVal());
                        this->addIndirectForkEdge(*it, callee);
                    }
                }
            }
        }
    }
}


/*!
 * Update join edge using pointer analysis results
 */
void ThreadCallGraph::updateJoinEdge(PointerAnalysis* pta) {

    for (CallSiteSet::iterator it = joinsitesBegin(), eit = joinsitesEnd(); it != eit; ++it) {
        const Value* jointhread = tdAPI->getJoinedThread(*it);
        // find its corresponding fork sites first
        CallSiteSet forkset;
        for (CallSiteSet::iterator it = forksitesBegin(), eit = forksitesEnd(); it != eit; ++it) {
            const Value* forkthread = tdAPI->getForkedThread(*it);
            if (pta->alias(jointhread, forkthread)) {
                forkset.insert(*it);
            }
        }
        assert(!forkset.empty() && "Can't find a forksite for this join!!");
        addDirectJoinEdge(*it,forkset);
    }
}

/*!
 * Add direct fork edges
 */
void ThreadCallGraph::addDirectForkEdge(const llvm::Instruction* call) {

    PTACallGraphNode* caller = getCallGraphNode(call->getParent()->getParent());
    const Function* forkee = dyn_cast<Function>(tdAPI->getForkedFun(call));
    assert(forkee && "callee does not exist");
    PTACallGraphNode* callee = getCallGraphNode(forkee);

    if (PTACallGraphEdge* callEdge = hasGraphEdge(caller, callee, PTACallGraphEdge::TDForkEdge)) {
        callEdge->addDirectCallSite(call);
        addThreadForkEdgeSetMap(call, cast<ThreadForkEdge>(callEdge));
    } else {
        assert(call->getParent()->getParent() == caller->getFunction() && "callee instruction not inside caller??");

        ThreadForkEdge* edge = new ThreadForkEdge(caller, callee);
        edge->addDirectCallSite(call);

        addEdge(edge);
        addThreadForkEdgeSetMap(call, edge);
    }
}

/*!
 * Add indirect fork edge to update call graph
 */
void ThreadCallGraph::addIndirectForkEdge(const llvm::Instruction* call, const llvm::Function* calleefun) {
    PTACallGraphNode* caller = getCallGraphNode(call->getParent()->getParent());
    PTACallGraphNode* callee = getCallGraphNode(calleefun);

    if (PTACallGraphEdge* callEdge = hasGraphEdge(caller, callee, PTACallGraphEdge::TDForkEdge)) {
        callEdge->addInDirectCallSite(call);
        addThreadForkEdgeSetMap(call, cast<ThreadForkEdge>(callEdge));
    } else {
        assert(call->getParent()->getParent() == caller->getFunction() && "callee instruction not inside caller??");

        ThreadForkEdge* edge = new ThreadForkEdge(caller, callee);
        edge->addInDirectCallSite(call);

        addEdge(edge);
        addThreadForkEdgeSetMap(call, edge);
    }
}

/*!
 * Add direct fork edges
 * As join edge is a special return which is back to join site(s) rather than its fork site
 * A ThreadJoinEdge is created from the functions where join sites reside in to the start routine function
 * But we don't invoke addEdge() method to add the edge to src and dst, otherwise it makes a scc cycle
 */
void ThreadCallGraph::addDirectJoinEdge(const llvm::Instruction* call,const CallSiteSet& forkset) {

    PTACallGraphNode* joinFunNode = getCallGraphNode(call->getParent()->getParent());

    for (CallSiteSet::const_iterator it = forkset.begin(), eit = forkset.end(); it != eit; ++it) {

        const CallInst* forksite = *it;
        const Function* threadRoutineFun = dyn_cast<Function>(tdAPI->getForkedFun(forksite));
        assert(threadRoutineFun && "thread routine function does not exist");
        PTACallGraphNode* threadRoutineFunNode = getCallGraphNode(threadRoutineFun);

        if (ThreadJoinEdge* joinEdge = hasThreadJoinEdge(call,joinFunNode,threadRoutineFunNode)) {
            joinEdge->addDirectCallSite(call);
            addThreadJoinEdgeSetMap(call, joinEdge);
        } else {
            assert(call->getParent()->getParent() == joinFunNode->getFunction() && "callee instruction not inside caller??");

            ThreadJoinEdge* edge = new ThreadJoinEdge(joinFunNode,threadRoutineFunNode);
            edge->addDirectCallSite(call);

            addThreadJoinEdgeSetMap(call, edge);
        }
    }
}
