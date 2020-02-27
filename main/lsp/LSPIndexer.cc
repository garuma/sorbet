#include "main/lsp/LSPIndexer.h"
#include "common/concurrency/ConcurrentQueue.h"
#include "core/ErrorQueue.h"
#include "core/Unfreeze.h"
#include "core/lsp/TypecheckEpochManager.h"
#include "main/lsp/LSPConfiguration.h"
#include "main/lsp/ShowOperation.h"
#include "main/pipeline/pipeline.h"

using namespace std;

namespace sorbet::realmain::lsp {
namespace {
const core::File &getOldFile(core::FileRef fref, const core::GlobalState &gs,
                             const UnorderedMap<int, shared_ptr<core::File>> evictedFiles) {
    const auto &it = evictedFiles.find(fref.id());
    if (it != evictedFiles.end()) {
        return *it->second;
    }
    ENFORCE(fref.exists());
    return fref.data(gs);
}

// Merges *oldEvictedFiles* into *newlyEvictedFiles*. Mutates newlyEvictedFiles.
void mergeEvictedFiles(const UnorderedMap<int, shared_ptr<core::File>> &oldEvictedFiles,
                       UnorderedMap<int, shared_ptr<core::File>> &newlyEvictedFiles) {
    // Keep the older of the two file versions. We want the file version just prior to the currently pending slow path.
    for (const auto &entry : oldEvictedFiles) {
        newlyEvictedFiles[entry.first] = move(entry.second);
    }
}
} // namespace

LSPIndexer::LSPIndexer(shared_ptr<const LSPConfiguration> config, unique_ptr<core::GlobalState> initialGS)
    : config(config), initialGS(move(initialGS)), emptyWorkers(WorkerPool::create(0, *config->logger)) {}

LSPIndexer::~LSPIndexer() {
    if (pendingTypecheckLatencyTimer != nullptr) {
        pendingTypecheckLatencyTimer->cancel();
    }
}

void LSPIndexer::computeFileHashes(const vector<shared_ptr<core::File>> &files, WorkerPool &workers) const {
    // Fast abort if all files have hashes.
    bool allFilesHaveHashes = true;
    for (const auto &f : files) {
        if (f != nullptr && f->getFileHash() == nullptr) {
            allFilesHaveHashes = false;
            break;
        }
    }
    if (allFilesHaveHashes) {
        return;
    }

    Timer timeit(config->logger, "computeFileHashes");
    shared_ptr<ConcurrentBoundedQueue<int>> fileq = make_shared<ConcurrentBoundedQueue<int>>(files.size());
    for (int i = 0; i < files.size(); i++) {
        auto copy = i;
        fileq->push(move(copy), 1);
    }

    auto &logger = *config->logger;
    logger.debug("Computing state hashes for {} files", files.size());

    shared_ptr<BlockingBoundedQueue<vector<pair<int, shared_ptr<core::FileHash>>>>> resultq =
        make_shared<BlockingBoundedQueue<vector<pair<int, shared_ptr<core::FileHash>>>>>(files.size());
    workers.multiplexJob("lspStateHash", [fileq, resultq, files, &logger]() {
        vector<pair<int, shared_ptr<core::FileHash>>> threadResult;
        int processedByThread = 0;
        int job;
        {
            for (auto result = fileq->try_pop(job); !result.done(); result = fileq->try_pop(job)) {
                if (result.gotItem()) {
                    processedByThread++;

                    if (!files[job] || files[job]->getFileHash() != nullptr) {
                        continue;
                    }

                    auto hash = pipeline::computeFileHash(files[job], logger);
                    threadResult.emplace_back(job, make_shared<core::FileHash>(move(hash)));
                }
            }
        }

        if (processedByThread > 0) {
            resultq->push(move(threadResult), processedByThread);
        }
    });

    {
        vector<pair<int, shared_ptr<core::FileHash>>> threadResult;
        for (auto result = resultq->wait_pop_timed(threadResult, WorkerPool::BLOCK_INTERVAL(), logger); !result.done();
             result = resultq->wait_pop_timed(threadResult, WorkerPool::BLOCK_INTERVAL(), logger)) {
            if (result.gotItem()) {
                for (auto &a : threadResult) {
                    files[a.first]->setFileHash(move(a.second));
                }
            }
        }
    }
}

void LSPIndexer::computeFileHashes(const vector<shared_ptr<core::File>> &files) const {
    computeFileHashes(files, *emptyWorkers);
}

FastPathDecision LSPIndexer::makeFastPathDecision(const std::vector<std::shared_ptr<core::File>> &changedFiles,
                                                  bool containsPendingTypecheckUpdates) const {
    Timer timeit(config->logger, "fast_path_decision");
    auto &logger = *config->logger;
    logger.debug("Trying to see if fast path is available after {} file changes", changedFiles.size());
    if (config->disableFastPath) {
        logger.debug("Taking slow path because fast path is disabled.");
        prodCategoryCounterInc("lsp.slow_path_reason", "fast_path_disabled");
        return FastPathDecision::SLOW;
    }

    const UnorderedMap<int, shared_ptr<core::File>> emptyMap;
    const UnorderedMap<int, shared_ptr<core::File>> &evictedFilesRef =
        containsPendingTypecheckUpdates ? evictedFiles : emptyMap;
    for (auto &f : changedFiles) {
        auto fref = initialGS->findFileByPath(f->path());
        if (!fref.exists()) {
            logger.debug("Taking slow path because {} is a new file", f->path());
            prodCategoryCounterInc("lsp.slow_path_reason", "new_file");
            return FastPathDecision::SLOW;
        } else {
            const auto &oldFile = getOldFile(fref, *initialGS, evictedFilesRef);
            ENFORCE(oldFile.getFileHash() != nullptr);
            ENFORCE(f->getFileHash() != nullptr);
            auto oldHash = *oldFile.getFileHash();
            auto newHash = *f->getFileHash();
            ENFORCE(oldHash.definitions.hierarchyHash != core::GlobalStateHash::HASH_STATE_NOT_COMPUTED);
            if (newHash.definitions.hierarchyHash == core::GlobalStateHash::HASH_STATE_INVALID) {
                logger.debug("Taking slow path because {} has a syntax error", f->path());
                prodCategoryCounterInc("lsp.slow_path_reason", "syntax_error");
                return FastPathDecision::SLOW;
            } else if (newHash.definitions.hierarchyHash != core::GlobalStateHash::HASH_STATE_INVALID &&
                       newHash.definitions.hierarchyHash != oldHash.definitions.hierarchyHash) {
                logger.debug("Taking slow path because {} has changed definitions", f->path());
                prodCategoryCounterInc("lsp.slow_path_reason", "changed_definition");
                return FastPathDecision::SLOW;
            }
        }
    }

    logger.debug("Taking fast path");
    return FastPathDecision::FAST;
}

FastPathDecision LSPIndexer::makeFastPathDecision(const LSPFileUpdates &edit,
                                                  bool containsPendingTypecheckUpdates) const {
    auto &logger = *config->logger;
    // Path taken after the first time an update has been encountered. Hack since we can't roll back new files just yet.
    if (edit.hasNewFiles) {
        logger.debug("Taking slow path because update has a new file");
        prodCategoryCounterInc("lsp.slow_path_reason", "new_file");
        return FastPathDecision::SLOW;
    }
    return makeFastPathDecision(edit.updatedFiles, containsPendingTypecheckUpdates);
}

void LSPIndexer::initialize(LSPFileUpdates &updates, WorkerPool &workers) {
    if (initialized) {
        Exception::raise("Indexer is already initialized; cannot initialize a second time.");
    }
    initialized = true;
    // Temporarily replace error queue, as it asserts that the same thread that created it uses it and we're
    // going to use it on typechecker thread for this one operation.
    auto savedErrorQueue = initialGS->errorQueue;
    initialGS->errorQueue = make_shared<core::ErrorQueue>(savedErrorQueue->logger, savedErrorQueue->tracer);
    initialGS->errorQueue->ignoreFlushes = true;

    vector<ast::ParsedFile> indexed;
    Timer timeit(config->logger, "initial_index");
    ShowOperation op(*config, "Indexing", "Indexing files...");
    {
        Timer timeit(config->logger, "reIndexFromFileSystem");
        vector<core::FileRef> inputFiles = pipeline::reserveFiles(initialGS, config->opts.inputFileNames);
        for (auto &t : pipeline::index(initialGS, inputFiles, config->opts, workers, kvstore)) {
            int id = t.file.id();
            if (id >= indexed.size()) {
                indexed.resize(id + 1);
            }
            indexed[id] = move(t);
        }
        // Clear error queue.
        // (Note: Flushing is disabled in LSP mode, so we have to drain.)
        initialGS->errorQueue->drainWithQueryResponses();
    }

    // When inputFileNames is 0 (as in tests), indexed ends up being size 0 because we don't index payload files.
    // At the same time, we expect indexed to be the same size as GlobalStateHash, which _does_ have payload files.
    // Resize the indexed array accordingly.
    if (indexed.size() < initialGS->getFiles().size()) {
        indexed.resize(initialGS->getFiles().size());
    }

    computeFileHashes(initialGS->getFiles(), workers);

    updates.epoch = 0;
    updates.fastPathDecision = FastPathDecision::SLOW;
    updates.updatedFileIndexes = move(indexed);
    updates.updatedGS = initialGS->deepCopy();

    // Restore error queue, as initialGS will be used on the LSPLoop thread from now on.
    initialGS->errorQueue = move(savedErrorQueue);
}

LSPFileUpdates LSPIndexer::commitEdit(unique_ptr<Timer> &latencyTimer, SorbetWorkspaceEditParams &edit,
                                      FastPathDecision cachedFastPathDecision) {
    Timer timeit(config->logger, "LSPIndexer::commitEdit");
    LSPFileUpdates update;
    update.epoch = edit.epoch;
    update.editCount = edit.mergeCount + 1;
    // Ensure all files have hashes.
    computeFileHashes(edit.updates, *emptyWorkers);

    update.updatedFiles = move(edit.updates);
    update.fastPathDecision = cachedFastPathDecision;
    if (update.fastPathDecision == FastPathDecision::NOT_DETERMINED) {
        update.fastPathDecision = makeFastPathDecision(update);
    }
    update.cancellationExpected = edit.sorbetCancellationExpected;
    update.preemptionsExpected = edit.sorbetPreemptionsExpected;

    UnorderedMap<int, shared_ptr<core::File>> newlyEvictedFiles;
    // Update globalStateHashes. Keep track of file IDs for these files, along with old hashes for these files.
    vector<core::FileRef> frefs;
    {
        core::UnfreezeFileTable fileTableAccess(*initialGS);
        int i = -1;
        for (auto &file : update.updatedFiles) {
            auto fref = initialGS->findFileByPath(file->path());
            i++;
            if (fref.exists()) {
                newlyEvictedFiles[fref.id()] = initialGS->getFiles()[fref.id()];
                initialGS = core::GlobalState::replaceFile(move(initialGS), fref, file);
            } else {
                // This file update adds a new file to GlobalState.
                update.hasNewFiles = true;
                fref = initialGS->enterFile(file);
                fref.data(*initialGS).strictLevel = pipeline::decideStrictLevel(*initialGS, fref, config->opts);
            }
            frefs.push_back(fref);
        }
    }

    // Index changes in initialGS. pipeline::index sorts output by file id, but we need to reorder to match the order of
    // other fields.
    UnorderedMap<u2, int> fileToPos;
    {
        int i = -1;
        for (auto fref : frefs) {
            // We should have ensured before reaching here that there are no duplicates.
            ENFORCE(!fileToPos.contains(fref.id()));
            i++;
            fileToPos[fref.id()] = i;
        }
    }

    {
        // Create a throwaway error queue. commitEdit may be called on two different threads, and we can't anticipate
        // which one it will be.
        initialGS->errorQueue =
            make_shared<core::ErrorQueue>(initialGS->errorQueue->logger, initialGS->errorQueue->tracer);
        initialGS->errorQueue->ignoreFlushes = true;
        auto trees = pipeline::index(initialGS, frefs, config->opts, *emptyWorkers, kvstore);
        initialGS->errorQueue->drainWithQueryResponses(); // Clear error queue; we don't care about errors here.
        update.updatedFileIndexes.resize(trees.size());
        for (auto &ast : trees) {
            const int i = fileToPos[ast.file.id()];
            update.updatedFileIndexes[i] = move(ast);
        }
    }

    auto runningSlowPath = initialGS->epochManager->getStatus();
    if (runningSlowPath.slowPathRunning) {
        // A cancelable slow path is currently running. Before running deepCopy(), check if we can cancel -- we might be
        // able to avoid it.
        // Invariant: `pendingTypecheckUpdates` should contain the edits currently being typechecked on the slow path.
        // runningSlowPath.epoch should be in the interval (pendingTypecheckUpdates.epoch - editCount,
        // pendingTypecheckUpdates.epoch]
        ENFORCE(runningSlowPath.epoch <= pendingTypecheckUpdates.epoch);
        ENFORCE(runningSlowPath.epoch > (pendingTypecheckUpdates.epoch - pendingTypecheckUpdates.editCount));

        // Cancel if the new update cannot take the fast path.
        if (update.fastPathDecision == FastPathDecision::SLOW &&
            initialGS->epochManager->tryCancelSlowPath(update.epoch)) {
            // Cancelation succeeded! Merge with the pending update.
            update.mergeOlder(pendingTypecheckUpdates);
            update.fastPathDecision = makeFastPathDecision(update, true);
            update.canceledSlowPath = true;
            mergeEvictedFiles(evictedFiles, newlyEvictedFiles);
        }
    }

    ENFORCE(update.updatedFiles.size() == update.updatedFileIndexes.size());

    ENFORCE(update.fastPathDecision != FastPathDecision::NOT_DETERMINED);
    if (update.fastPathDecision == FastPathDecision::FAST) {
        // Edit takes the fast path. Merge with this edit so we can reverse it if the slow path gets canceled.
        auto merged = update.copy();
        merged.mergeOlder(pendingTypecheckUpdates);
        pendingTypecheckUpdates = move(merged);
        if (update.canceledSlowPath && pendingTypecheckLatencyTimer != nullptr) {
            // Replace edit's latencyTimer with that of the running slow path.
            if (latencyTimer != nullptr) {
                latencyTimer->cancel();
            }
            latencyTimer = make_unique<Timer>(pendingTypecheckLatencyTimer->clone());
        }
        mergeEvictedFiles(evictedFiles, newlyEvictedFiles);
    } else {
        // Completely replace `pendingTypecheckUpdates` if this was a slow path update.
        update.updatedGS = initialGS->deepCopy();
        pendingTypecheckUpdates = update.copy();
        if (pendingTypecheckLatencyTimer != nullptr) {
            pendingTypecheckLatencyTimer->cancel();
        }
        if (latencyTimer != nullptr) {
            pendingTypecheckLatencyTimer = make_unique<Timer>(latencyTimer->clone());
        }
    }

    // newlyEvictedFiles contains the changes from this edit + changes from the pending typecheck, if applicable.
    evictedFiles = std::move(newlyEvictedFiles);

    // Don't copy over these (test-only) properties, as they only apply to the original request.
    pendingTypecheckUpdates.cancellationExpected = false;
    pendingTypecheckUpdates.preemptionsExpected = 0;

    return update;
}

} // namespace sorbet::realmain::lsp