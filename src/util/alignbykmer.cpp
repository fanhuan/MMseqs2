#include "DistanceCalculator.h"
#include "Util.h"
#include "Parameters.h"
#include "Matcher.h"
#include "Debug.h"
#include "DBReader.h"
#include "DBWriter.h"
#include "QueryMatcher.h"
#include "NucleotideMatrix.h"
#include "ReducedMatrix.h"
#include "ExtendedSubstitutionMatrix.h"

#include <string>
#include <vector>

#ifdef OPENMP
#include <omp.h>
#endif

void setAlignByKmerDefaults(Parameters *p) {
    p->kmerSize = 4;
    p->alphabetSize = 21;
}

int alignbykmer(int argc, const char **argv, const Command &command) {
    Debug(Debug::INFO) << "Rescore diagonals.\n";
    Parameters &par = Parameters::getInstance();
    setAlignByKmerDefaults(&par);
    par.parseParameters(argc, argv, command, 4);
#ifdef OPENMP
    omp_set_num_threads(par.threads);
#endif


    Debug(Debug::INFO) << "Query  file: " << par.db1 << "\n";
    DBReader<unsigned int> *qdbr = new DBReader<unsigned int>(par.db1.c_str(), (par.db1 + ".index").c_str());
    qdbr->open(DBReader<unsigned int>::NOSORT);
    const int querySeqType = qdbr->getDbtype();
    qdbr->readMmapedDataInMemory();

    BaseMatrix *subMat;
    if (querySeqType == Sequence::NUCLEOTIDES) {
        subMat = new NucleotideMatrix(par.scoringMatrixFile.c_str(), 1.0, 0.0);
    } else {
        if (par.alphabetSize == 21) {
            subMat = new SubstitutionMatrix(par.scoringMatrixFile.c_str(), 2.0, 0.0);
        } else {
            SubstitutionMatrix sMat(par.scoringMatrixFile.c_str(), 2.0, 0.0);
            subMat = new ReducedMatrix(sMat.probMatrix, sMat.subMatrixPseudoCounts, sMat.aa2int, sMat.int2aa,
                    sMat.alphabetSize, par.alphabetSize, 2.0);
            SubstitutionMatrix::print(subMat->subMatrix, subMat->int2aa, subMat->alphabetSize );
        }
    }
    ScoreMatrix * _2merSubMatrix =  ExtendedSubstitutionMatrix::calcScoreMatrix(*subMat, 2);

    Debug(Debug::INFO) << "Target  file: " << par.db2 << "\n";
    DBReader<unsigned int> *tdbr = NULL;
    bool sameDB = false;
    if (par.db1.compare(par.db2) == 0) {
        sameDB = true;
        tdbr = qdbr;
    } else {
        tdbr = new DBReader<unsigned int>(par.db2.c_str(), par.db2Index.c_str());
        tdbr->open(DBReader<unsigned int>::NOSORT);
        tdbr->readMmapedDataInMemory();
    }

    EvalueComputation evaluer(tdbr->getAminoAcidDBSize(), subMat, par.gapOpen, par.gapExtend);

    Debug(Debug::INFO) << "Prefilter database: " << par.db3 << "\n";
    DBReader<unsigned int> dbr_res(par.db3.c_str(), par.db3Index.c_str());
    dbr_res.open(DBReader<unsigned int>::LINEAR_ACCCESS);

    Debug(Debug::INFO) << "Result database: " << par.db4 << "\n";
    DBWriter resultWriter(par.db4.c_str(), par.db4Index.c_str(), par.threads);
    resultWriter.open();

    struct KmerPos {
        unsigned short ij;
        unsigned short i;
        unsigned short j;
        KmerPos(unsigned int ij, unsigned short i, unsigned short j):
                ij(ij), i(i), j(j) {}
        KmerPos() {};
        // need for sorting the results
        static bool compareKmerPos(const KmerPos &first, const KmerPos &second) {
            //return (first.eval < second.eval);
            if (first.ij  < second.ij)
                return true;
            if (second.ij < first.ij)
                return false;
            if (first.i   < second.i)
                return true;
            if (second.i  < first.i)
                return false;
            if (first.j   < second.j)
                return true;
            if (second.j  < first.j)
                return false;
            return false;
        }
    };


    struct Stretche {
        unsigned short i_start;
        unsigned short i_end;
        unsigned short j_start;
        unsigned short j_end;
        unsigned short kmerCnt;

        Stretche(unsigned short i_start, unsigned short i_end,
                 unsigned short j_start, unsigned short j_end, unsigned short kmerCnt):
                i_start(i_start), i_end(i_end), j_start(j_start), j_end(j_end), kmerCnt(kmerCnt) {}
        Stretche() {};
        // need for sorting the results
        static bool compareStretche(const Stretche &first, const Stretche &second) {
            //return (first.eval < second.eval);
            if (first.i_start < second.i_start)
                return true;
            if (first.i_start > second.i_start)
                return false;
            if (first.i_end > second.i_end)
                return true;
            if (first.i_end < second.i_end)
                return false;
            return false;
        }
    };

    struct DpMatrixRow {
        DpMatrixRow() {}
        DpMatrixRow(size_t prevPotentialId, int pathScore)
                : prevPotentialId(prevPotentialId), pathScore(pathScore) {}
        size_t prevPotentialId;
        int pathScore;
    };


    size_t totalMemory = Util::getTotalSystemMemory();
    size_t flushSize = 100000000;
    if (totalMemory > dbr_res.getDataSize()) {
        flushSize = dbr_res.getSize();
    }
    size_t iterations = static_cast<int>(ceil(static_cast<double>(dbr_res.getSize()) / static_cast<double>(flushSize)));
    for (size_t i = 0; i < iterations; i++) {
        size_t start = (i * flushSize);
        size_t bucketSize = std::min(dbr_res.getSize() - (i * flushSize), flushSize);
#pragma omp parallel
        {
            Sequence query(par.maxSeqLen, querySeqType, subMat, par.kmerSize, par.spacedKmer, false, true, par.spacedKmerPattern);
            Sequence target(par.maxSeqLen, querySeqType, subMat, par.kmerSize, par.spacedKmer, false, true, par.spacedKmerPattern);
            KmerGenerator kmerGenerator(par.kmerSize, subMat->alphabetSize, 70.0);
            kmerGenerator.setDivideStrategy(NULL, _2merSubMatrix);
            size_t lookupSize = MathUtil::ipow<size_t>(par.alphabetSize, par.kmerSize);
            unsigned short * queryPosLookup = new unsigned short[lookupSize];
            memset(queryPosLookup, 255, lookupSize * sizeof(unsigned short) );

            Indexer idxer(subMat->alphabetSize, par.kmerSize);
            KmerPos * kmerPosVec = new KmerPos[par.maxSeqLen];
            Stretche * stretcheVec = new Stretche[par.maxSeqLen];
            DpMatrixRow * dpMatrixRow = new DpMatrixRow[par.maxSeqLen];
            int * scores = new int[par.maxSeqLen];
            std::string bt;

            unsigned int thread_idx = 0;
#ifdef OPENMP
            thread_idx = (unsigned int) omp_get_thread_num();
#endif
            char buffer[1024 + 32768];

#pragma omp for schedule(dynamic, 1)
            for (size_t id = start; id < (start + bucketSize); id++) {
                Debug::printProgress(id);
                std::string prefResultsOutString;
                prefResultsOutString.reserve(1000000);

                char *data = dbr_res.getData(id);
                unsigned int queryId = qdbr->getId(dbr_res.getDbKey(id));
                char *querySeq = qdbr->getData(queryId);
                query.mapSequence(id, queryId, querySeq);

                while (query.hasNextKmer()) {
                    const int *kmer = query.nextKmer();
                    unsigned short pos = query.getCurrentPosition();
                    unsigned short kmerIdx = idxer.int2index(kmer);
                    if (queryPosLookup[kmerIdx] == USHRT_MAX) {
                        queryPosLookup[kmerIdx] = pos;
                    }
//                    ScoreMatrix scoreMat = kmerGenerator.generateKmerList(kmer);
//                    for (size_t pos = 0; pos <  scoreMat.elementSize; pos++) {
//                        if (queryPosLookup[scoreMat.index[pos]] == USHRT_MAX) {
//                            queryPosLookup[scoreMat.index[pos]] = pos;
//                        }
//                    }
                }
                resultWriter.writeStart(thread_idx);

                while (*data != '\0') {
                    // DB key of the db sequence
                    char dbKeyBuffer[255 + 1];
                    Util::parseKey(data, dbKeyBuffer);
                    const unsigned int dbKey = (unsigned int) strtoul(dbKeyBuffer, NULL, 10);
                    unsigned int targetId = tdbr->getId(dbKey);
                    char *targetSeq = tdbr->getData(targetId);
                    const bool isIdentity = (queryId == targetId && (par.includeIdentity || sameDB)) ? true : false;
                    target.mapSequence(targetId, dbKey, targetSeq);
                    size_t kmerPosSize = 0;
                    while (target.hasNextKmer()) {
                        const int *kmer = target.nextKmer();
                        unsigned short kmerIdx = idxer.int2index(kmer);
                        if (queryPosLookup[kmerIdx] != USHRT_MAX) {
                            unsigned short pos_j = target.getCurrentPosition();
                            unsigned short pos_i = queryPosLookup[kmerIdx];
                            unsigned short ij = pos_i - pos_j;
                            kmerPosVec[kmerPosSize].ij = ij;
                            kmerPosVec[kmerPosSize].i  = pos_i;
                            kmerPosVec[kmerPosSize].j  = pos_j;
                            kmerPosSize++;
                        }
                    }


                    std::sort(kmerPosVec, kmerPosVec + kmerPosSize, KmerPos::compareKmerPos);
                    unsigned short region_min_i = USHRT_MAX;
                    unsigned short region_max_i = 0;
                    unsigned short region_min_j = USHRT_MAX;
                    unsigned short region_max_j = 0;
                    unsigned short region_max_kmer_cnt = 0;
                    size_t stretcheSize = 0;
                    if (kmerPosSize > 1) {
                        unsigned int prevDiagonal = UINT_MAX;
                        unsigned short prev_i = 0;
                        unsigned short prev_j = 0;

                        for (size_t kmerIdx = 0; kmerIdx < kmerPosSize; kmerIdx++) {
                            unsigned int currDiagonal = static_cast<unsigned int>(kmerPosVec[kmerIdx].i) - static_cast<unsigned int>(kmerPosVec[kmerIdx].j);
                            unsigned short curr_i = kmerPosVec[kmerIdx].i;
                            unsigned short curr_j = kmerPosVec[kmerIdx].j;
                            unsigned int nextDiagonal = UINT_MAX;
                            if (kmerIdx < (kmerPosSize - 1)) {
                                nextDiagonal = static_cast<unsigned int>(kmerPosVec[kmerIdx+1].i) - static_cast<unsigned int>(kmerPosVec[kmerIdx+1].j);
                            }
                            // skip single hits
                            if (currDiagonal != nextDiagonal && currDiagonal != prevDiagonal) {
                                continue;
                            }


                            if ((nextDiagonal == currDiagonal || prevDiagonal == currDiagonal)
                               && prev_i <= curr_i && prev_j <= curr_j) {
//                std::cout << "ij: " << kmerPosVec[kmerIdx].ij << " i: " << kmerPosVec[kmerIdx].i
//                      << " j:" << kmerPosVec[kmerIdx].j << " Diag: " <<  kmerPosVec[kmerIdx].i - kmerPosVec[kmerIdx].j << std::endl;

                                region_min_i = std::min(region_min_i, curr_i);
                                region_max_i = std::max(region_max_i, curr_i);
                                region_min_j = std::min(region_min_j, curr_j);
                                region_max_j = std::max(region_max_j, curr_j);
                                region_max_kmer_cnt++;
                            }
                            prevDiagonal = currDiagonal;
                            prev_i=curr_i;
                            prev_j=curr_j;
                            if (nextDiagonal != currDiagonal || kmerIdx == (kmerPosSize - 1)) {
//                                std::cout << region_min_i << "\t" << region_max_i << "\t" << region_min_j << "\t"  << region_max_j << "\t" << region_min_i - region_min_j << std::endl;
                                stretcheVec[stretcheSize].i_start = region_min_i;
                                stretcheVec[stretcheSize].i_end = region_max_i;
                                stretcheVec[stretcheSize].j_start = region_min_j;
                                stretcheVec[stretcheSize].j_end = region_max_j;
                                stretcheVec[stretcheSize].kmerCnt = region_max_kmer_cnt;

                                stretcheSize++;
                                region_min_i = USHRT_MAX;
                                region_max_i = 0;
                                region_min_j = USHRT_MAX;
                                region_max_j = 0;
                                region_max_kmer_cnt = 0;
                                prev_i=0;
                                prev_j=0;
                            }
                        }
                    }
                    // Do dynamic programming
                    std::sort(stretcheVec, stretcheVec + stretcheSize, Stretche::compareStretche);
                    for (size_t id = 0; id < stretcheSize; ++id) {
                        dpMatrixRow[id].prevPotentialId = id;
                        dpMatrixRow[id].pathScore = stretcheVec[id].kmerCnt;
                    }
                    int bestPathScore = 0;
                    size_t lastPotentialExonInBestPath = 0;
                    for (size_t currStretche = 0; currStretche < stretcheSize; ++currStretche) {
                        for (size_t prevPotentialStretche = 0; prevPotentialStretche < currStretche; ++prevPotentialStretche) {
                            // check the first one does not contain the second one:
                            if (stretcheVec[currStretche].i_start > stretcheVec[prevPotentialStretche].i_end &&
                                    stretcheVec[currStretche].j_start > stretcheVec[prevPotentialStretche].i_end) {
                                int bestScorePathPrevIsLast = dpMatrixRow[prevPotentialStretche].pathScore;
                                int distance =  -1;
                                int costOfPrevToCurrTransition = distance;
                                int currScore = stretcheVec[currStretche].kmerCnt;
                                int currScoreWithPrev = bestScorePathPrevIsLast + costOfPrevToCurrTransition + currScore;

                                // update row of currPotentialExon in case of improvement:
                                if (currScoreWithPrev > dpMatrixRow[currStretche].pathScore) {
                                    dpMatrixRow[currStretche].prevPotentialId = prevPotentialStretche;
                                    dpMatrixRow[currStretche].pathScore = currScoreWithPrev;
                                }
                            }
                        }

                        // update the global max in case of improvement:
                        if (dpMatrixRow[currStretche].pathScore > bestPathScore) {
                            lastPotentialExonInBestPath = currStretche;
                            bestPathScore = dpMatrixRow[currStretche].pathScore;
                        }
                    }

                    size_t currId = lastPotentialExonInBestPath;
                    std::vector<Stretche> strechtPath;
                    while (dpMatrixRow[currId].prevPotentialId != currId) {
                        strechtPath.emplace_back(stretcheVec[currId]);
                        currId = dpMatrixRow[currId].prevPotentialId;
                    }
                    strechtPath.emplace_back(stretcheVec[currId]);
                    // do 1d dp to find optimal transition point
                    for (size_t stretch = (strechtPath.size() - 1); stretch > 0; stretch--) {
                        int score = 0;
                        int pos = 0;
//                        query.print();
//                        target.print();

//                        for (int i = strechtPath[stretch].i_end, j = strechtPath[stretch].j_end;
//                             i < strechtPath[stretch - 1].i_start; i++, j++) {
//                            std::cout << subMat->int2aa[query.int_sequence[i]];
//                        }
//                        std::cout << std::endl;
//
//                        for (int i = strechtPath[stretch].i_end, j = strechtPath[stretch].j_end;
//                             i < strechtPath[stretch - 1].i_start; i++, j++) {
//                            std::cout << subMat->int2aa[target.int_sequence[j]];
//                        }
//                        std::cout << std::endl;

                        for (int i = strechtPath[stretch].i_end, j = strechtPath[stretch].j_end;
                             i < strechtPath[stretch - 1].i_start && j < strechtPath[stretch - 1].j_start; i++, j++) {
                            int curr = subMat->subMatrix[query.int_sequence[i]][target.int_sequence[j]];
                            score = curr + score;
//                            score = (score < 0) ? 0 : score;
                            scores[pos] = score;
                            pos++;
                        }
                        int maxScore = 0;
                        int maxPos = 0;
                        int maxRevPos = 0;
                        int revPos = 0;
                        scores[pos] = 0;
                        score = 0;
                        for (int i = strechtPath[stretch - 1].i_start, j = strechtPath[stretch - 1].j_start;
                             i > strechtPath[stretch].i_end && j > strechtPath[stretch].j_end; i--, j--) {
                            int curr = subMat->subMatrix[query.int_sequence[i]][target.int_sequence[j]];
                            score = curr + score;
//                            score = (score < 0) ? 0 : score;
                            if (scores[pos] + score > maxScore) {
                                maxScore = scores[pos] + score;
                                maxPos = pos;
                                maxRevPos = revPos;
                            }
                            revPos++;
                            pos--;
                        }
                        strechtPath[stretch - 1].i_start = strechtPath[stretch - 1].i_start - maxRevPos;
                        strechtPath[stretch - 1].j_start = strechtPath[stretch - 1].j_start - maxRevPos;
                        strechtPath[stretch].i_end = strechtPath[stretch].i_end + maxPos;
                        strechtPath[stretch].j_end = strechtPath[stretch].j_end + maxPos;
                    }

                    // find correct start and end
                    {
                        int maxScore = 0;
                        int score = 0;
                        for (int i = strechtPath[strechtPath.size()-1].i_start, j = strechtPath[strechtPath.size()-1].j_start; i > -1 && j > -1; i--, j--) {
                            int curr = subMat->subMatrix[query.int_sequence[i]][target.int_sequence[j]];
                            score = curr + score;
                            //                            score = (score < 0) ? 0 : score;
                            if (score > maxScore) {
                                strechtPath[strechtPath.size()-1].i_start = i;
                                strechtPath[strechtPath.size()-1].j_start = j;
                            }
                        }
                        score = 0;

                        for (int i = strechtPath[0].i_end, j = strechtPath[0].j_end; i < query.L && j < target.L; i++, j++) {
                            int curr = subMat->subMatrix[query.int_sequence[i]][target.int_sequence[j]];
                            score = curr + score;
                            //                            score = (score < 0) ? 0 : score;
                            if (score > maxScore) {
                                strechtPath[0].i_end = i;
                                strechtPath[0].j_end = j;
                            }
                        }
                    }
//                    for(size_t stretch = 0; stretch < strechtPath.size() ; stretch++) {
//                        std::cout << stretcheVec[stretch].i_start << "\t" << stretcheVec[stretch].i_end << "\t";
//                    }
//                    std::cout << std::endl;

//                    std::string querystr;
//                    std::string targetstr;
                    int ids = 0;
                    int score = 0;

                    for (int stretch = strechtPath.size()-1; stretch > -1 ; stretch--) {
                        for (size_t i = strechtPath[stretch].i_start, j = strechtPath[stretch].j_start;
                               i < strechtPath[stretch].i_end; i++, j++) {
//                            querystr.push_back(subMat->int2aa[query.int_sequence[i]]);
//                            targetstr.push_back(subMat->int2aa[target.int_sequence[j]]);
                            bt.push_back('M');
                            ids += (query.int_sequence[i] == target.int_sequence[j]);
                            score += subMat->subMatrix[query.int_sequence[i]][target.int_sequence[j]];
                        }
                        if (stretch > 0) {
                            score -= par.gapOpen;
                            if (strechtPath[stretch-1].i_start==strechtPath[stretch].i_end) {
                                for (size_t pos = strechtPath[stretch].j_end; pos < strechtPath[stretch-1].j_start; pos++) {
//                                    querystr.push_back('-');
//                                    targetstr.push_back(subMat->int2aa[target.int_sequence[pos]]);
                                    bt.push_back('I');
                                    score -= par.gapExtend;
                                }
                            } else {
                                for (size_t pos = strechtPath[stretch].i_end; pos < strechtPath[stretch-1].i_start; pos++) {
//                                    querystr.push_back(subMat->int2aa[query.int_sequence[pos]]);
//                                    targetstr.push_back('-');
                                    bt.push_back('D');
                                    score -= par.gapExtend;
                                }
                            }
                        }
                    }

//                    std::cout << querystr << std::endl;
//                    std::cout << targetstr << std::endl;
                    float queryCov = SmithWaterman::computeCov(strechtPath[strechtPath.size()-1].i_start , strechtPath[0].i_end,  query.L);
                    float targetCov = SmithWaterman::computeCov(strechtPath[strechtPath.size()-1].j_start , strechtPath[0].j_end, target.L);
                    int alnLen = bt.size();

                    const float seqId = static_cast<float>(ids)/static_cast<float>(alnLen);

                    int bitScore = static_cast<short>(evaluer.computeBitScore(score)+0.5);

                    const double evalue = evaluer.computeEvalue(score, query.L);
                    // query/target cov mode
                    const bool hasCov = Util::hasCoverage(par.covThr, par.covMode, queryCov, targetCov);
                    // --min-seq-id
                    const bool hasSeqId = seqId >= (par.seqIdThr - std::numeric_limits<float>::epsilon());
                    const bool hasEvalue = (evalue <= par.evalThr);
                    if (isIdentity || (hasCov && hasSeqId && hasEvalue)) {
                        Matcher::result_t result = Matcher::result_t(dbKey, bitScore, queryCov, targetCov, seqId, evalue,
                                                                     alnLen,
                                                                     strechtPath[strechtPath.size()-1].i_start , strechtPath[0].i_end, query.L, strechtPath[strechtPath.size()-1].j_start, strechtPath[0].j_end,
                                                                     target.L, bt);
                        size_t len = Matcher::resultToBuffer(buffer, result, true, true);
                        resultWriter.writeAdd(buffer, len, thread_idx);
                    }
                    data = Util::skipLine(data);
                    bt.clear();
                }
                memset(queryPosLookup, 255, lookupSize * sizeof(unsigned short));
                resultWriter.writeEnd(qdbr->getDbKey(queryId), thread_idx, true);
            }
            delete [] kmerPosVec;
            delete [] queryPosLookup;
            delete [] dpMatrixRow;
            delete [] scores;
        }
        dbr_res.remapData();
    }
    Debug(Debug::INFO) << "Done." << "\n";
    resultWriter.close();
    dbr_res.close();
    qdbr->close();

    delete qdbr;
    delete subMat;
    ScoreMatrix::cleanup(_2merSubMatrix);

    if (sameDB == false) {
        tdbr->close();
        delete tdbr;
    }

    return EXIT_SUCCESS;
}


