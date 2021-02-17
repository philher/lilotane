
#include "fact_analysis.h"

SigSet FactAnalysis::getPossibleFactChanges(const USignature& sig, FactInstantiationMode mode, OperationType opType) {
    
    if (sig == Sig::NONE_SIG) return SigSet();
    if (opType == UNKNOWN) opType = _htn.isAction(sig) ? ACTION : REDUCTION;
    
    if (opType == ACTION) {
        return _htn.getAction(sig).getEffects();
    }
    if (_fact_changes_cache.count(sig)) {
        SigSet result = std::move(_fact_changes_cache[sig]);
        _fact_changes_cache.erase(sig);
        return result;
    }

    int nameId = sig._name_id;
    
    // Substitution mapping
    std::vector<int> placeholderArgs;
    USignature normSig = _htn.getNormalizedLifted(sig, placeholderArgs);
    Substitution sFromPlaceholder(placeholderArgs, sig._args);

    auto& factChanges = mode == FULL ? _fact_changes : _lifted_fact_changes;
    if (!factChanges.count(nameId)) {
        // Compute fact changes for origSig
        
        NodeHashSet<Signature, SignatureHasher> facts;

        if (_htn.isActionRepetition(nameId)) {
            // Special case: Action repetition
            Action a = _htn.getActionFromRepetition(nameId);
            a = a.substitute(Substitution(a.getArguments(), placeholderArgs));
            for (const Signature& eff : a.getEffects()) {
                facts.insert(eff);
            }
        } else {
            // Normal traversal to find possible fact changes
            _traversal.traverse(normSig.substitute(Substitution(normSig._args, placeholderArgs)), 
            NetworkTraversal::TRAVERSE_PREORDER,
            [&](const USignature& nodeSig, int depth) { // NOLINT
                Action a;
                if (_htn.isAction(nodeSig)) {
                    if (_htn.isActionRepetition(nameId)) {
                        // Special case: Action repetition
                        a = _htn.toAction(_htn.getActionNameFromRepetition(nameId), nodeSig._args);
                    } else {
                        a = _htn.toAction(nodeSig._name_id, nodeSig._args);
                    }
                } else if (_htn.hasSurrogate(nodeSig._name_id)) {
                    a = _htn.getSurrogate(nodeSig._name_id);
                    a = a.substitute(Substitution(a.getArguments(), nodeSig._args));
                }

                for (const Signature& eff : a.getEffects()) {
                    facts.insert(eff);
                }
            });
        }

        // Convert result to vector
        SigSet& liftedResult = _lifted_fact_changes[nameId];
        SigSet& result = _fact_changes[nameId];
        for (const Signature& sig : facts) {
            liftedResult.insert(sig);
            if (sig._usig._args.empty()) result.insert(sig);
            else for (const USignature& sigGround : ArgIterator::getFullInstantiation(sig._usig, _htn)) {
                result.emplace(sigGround, sig._negated);
            }
        }
    }

    // Get fact changes, substitute arguments
    SigSet out = factChanges.at(nameId);
    for (Signature& sig : out) {
        sig.apply(sFromPlaceholder);
    }

    _fact_changes_cache[sig] = out;
    return out;
}


std::vector<FlatHashSet<int>> FactAnalysis::getReducedArgumentDomains(const HtnOp& op) {

    const std::vector<int>& args = op.getArguments();
    const std::vector<int>& sorts = _htn.getSorts(op.getNameId());
    std::vector<FlatHashSet<int>> domainPerVariable(args.size());
    std::vector<bool> occursInPreconditions(args.size(), false);

    // Check each precondition regarding its valid decodings w.r.t. current state
    //const SigSet* preSets[2] = {&op.getPreconditions(), &op.getExtraPreconditions()};
    const SigSet* preSets[1] = {&op.getPreconditions()};
    for (const auto& preSet : preSets) for (const auto& preSig : *preSet) {

        // Find mapping from precond args to op args
        std::vector<int> opArgIndices(preSig._usig._args.size(), -1);
        for (size_t preIdx = 0; preIdx < preSig._usig._args.size(); preIdx++) {
            const int& arg = preSig._usig._args[preIdx];
            for (size_t i = 0; i < args.size(); i++) {
                if (args[i] == arg) {
                    opArgIndices[preIdx] = i;
                    occursInPreconditions[i] = true;
                    break;
                }
            }
        }

        if (!_htn.hasQConstants(preSig._usig) && _htn.isFullyGround(preSig._usig)) {
            // Check base condition; if unsatisfied, discard op 
            if (!isReachable(preSig)) return std::vector<FlatHashSet<int>>();
            // Add constants to respective argument domains
            for (size_t i = 0; i < preSig._usig._args.size(); i++) {
                domainPerVariable[opArgIndices[i]].insert(preSig._usig._args[i]);
            }
            continue;
        }

        // Compute sorts of the condition's args w.r.t. op signature
        std::vector<int> preSorts(preSig._usig._args.size());
        for (size_t i = 0; i < preSorts.size(); i++) {
            preSorts[i] = sorts[opArgIndices[i]];
        }

        // Check possible decodings of precondition
        bool any = false;
        bool anyValid = false;
        for (const auto& decUSig : _htn.decodeObjects(preSig._usig, /*restrictiveSorts=*/preSorts)) {
            any = true;
            assert(_htn.isFullyGround(decUSig));

            // Valid?
            if (!isReachable(decUSig, preSig._negated)) continue;
            
            // Valid precondition decoding found: Increase domain of concerned variables
            anyValid = true;
            for (size_t i = 0; i < opArgIndices.size(); i++) {
                int opArgIdx = opArgIndices[i];
                if (opArgIdx >= 0) {
                    domainPerVariable[opArgIdx].insert(decUSig._args[i]);
                }
            }
        }
        if (any && !anyValid) return std::vector<FlatHashSet<int>>();
    }

    for (size_t i = 0; i < args.size(); i++) {
        if (!occursInPreconditions[i]) domainPerVariable[i] = _htn.getConstantsOfSort(sorts[i]);
    }

    return domainPerVariable;
}

FactFrame FactAnalysis::getFactFrame(const USignature& sig, bool simpleMode, USigSet& currentOps) {
    static USigSet EMPTY_USIG_SET;

    //Log::d("GET_FACT_FRAME %s\n", TOSTR(sig));

    int nameId = sig._name_id;
    if (!_fact_frames.count(nameId)) {

        FactFrame result;

        std::vector<int> newArgs(sig._args.size());
        for (size_t i = 0; i < sig._args.size(); i++) {
            newArgs[i] = _htn.nameId("c" + std::to_string(i));
        }
        USignature op(sig._name_id, std::move(newArgs));
        result.sig = op;

        if (_htn.isAction(op)) {

            // Action
            const Action& a = _htn.toAction(op._name_id, op._args);
            result.preconditions = a.getPreconditions();
            result.flatEffects = a.getEffects();
            if (!simpleMode) result.causalEffects[std::vector<Signature>()] = a.getEffects();

        } else if (currentOps.count(op)) {

            // Handle recursive call of same reduction: Conservatively add preconditions and effects
            // without recursing on subtasks
            const Reduction& r = _htn.toReduction(op._name_id, op._args);
            result.preconditions = r.getPreconditions();
            result.flatEffects = getPossibleFactChanges(r.getSignature(), LIFTED);
            Log::d("RECURSIVE_FACT_FRAME %s\n", TOSTR(result.flatEffects));
            if (!simpleMode) result.causalEffects[std::vector<Signature>()] = result.flatEffects;

        } else {

            currentOps.insert(op);
            const Reduction& r = _htn.toReduction(op._name_id, op._args);
            result.sig = op;
            result.preconditions.insert(r.getPreconditions().begin(), r.getPreconditions().end());
            
            // For each subtask position ("offset")
            for (size_t offset = 0; offset < r.getSubtasks().size(); offset++) {
                
                FactFrame frameOfOffset;
                std::vector<USignature> children;
                _traversal.getPossibleChildren(r.getSubtasks(), offset, children);
                bool firstChild = true;

                // Assemble fact frame of this offset by iterating over all possible children at the offset
                for (const auto& child : children) {

                    // Assemble unified argument names
                    std::vector<int> newChildArgs(child._args);
                    for (size_t i = 0; i < child._args.size(); i++) {
                        if (_htn.isVariable(child._args[i])) newChildArgs[i] = _htn.nameId("??_");
                    }

                    // Recursively get child frame of the child
                    FactFrame childFrame = getFactFrame(USignature(child._name_id, std::move(newChildArgs)), simpleMode, EMPTY_USIG_SET);
                    
                    if (firstChild) {
                        // Add all preconditions of child that are not yet part of the parent's effects
                        for (const auto& pre : childFrame.preconditions) {
                            bool isNew = true;
                            for (const auto& eff : result.flatEffects) {
                                if (_htn.isUnifiable(eff, pre) || _htn.isUnifiable(pre, eff)) {
                                    isNew = false;
                                    Log::d("FACT_FRAME Precondition %s absorbed by effect %s of %s\n", TOSTR(pre), TOSTR(eff), TOSTR(child));
                                    break;
                                } 
                            }
                            if (isNew) frameOfOffset.preconditions.insert(std::move(pre));
                        }
                        firstChild = false;
                    } else {
                        // Intersect preconditions
                        SigSet newPrec;
                        for (auto& pre : childFrame.preconditions) {
                            if (frameOfOffset.preconditions.count(pre)) {
                                newPrec.insert(std::move(pre));
                            }
                        }
                        frameOfOffset.preconditions = std::move(newPrec);
                    }

                    if (simpleMode) {
                        // Add all of the child's effects to the parent's effects
                        frameOfOffset.flatEffects.insert(childFrame.flatEffects.begin(), childFrame.flatEffects.end());
                    } else {
                        // Add all effects with (child's precondition + eff. preconditions) minus the parent's effects
                        for (const auto& [pres, effs] : childFrame.causalEffects) {
                            SigSet newPres;
                            for (const auto& pre : childFrame.preconditions) {
                                if (!result.flatEffects.count(pre) && !frameOfOffset.preconditions.count(pre)) 
                                    newPres.insert(pre);
                            }
                            for (const auto& pre : pres) {
                                if (!result.flatEffects.count(pre) && !frameOfOffset.preconditions.count(pre)) 
                                    newPres.insert(pre);
                            }
                            frameOfOffset.causalEffects[std::vector<Signature>(newPres.begin(), newPres.end())] = effs;
                            frameOfOffset.flatEffects.insert(effs.begin(), effs.end());
                        }
                    }
                }

                // Write into parent's fact frame
                result.preconditions.insert(frameOfOffset.preconditions.begin(), frameOfOffset.preconditions.end());
                if (simpleMode) {
                    result.flatEffects.insert(frameOfOffset.flatEffects.begin(), frameOfOffset.flatEffects.end());
                } else {
                    for (const auto& [pres, effs] : frameOfOffset.causalEffects) {
                        result.causalEffects[pres].insert(effs.begin(), effs.end());
                        result.flatEffects.insert(effs.begin(), effs.end());
                    }
                }
            }
        }

        _fact_frames[nameId] = std::move(result);
        currentOps.erase(sig);

        //Log::d("FACT_FRAME %s\n", TOSTR(_fact_frames[nameId]));
    }

    const FactFrame& f = _fact_frames[nameId];
    return f.substitute(Substitution(f.sig._args, sig._args));
}
