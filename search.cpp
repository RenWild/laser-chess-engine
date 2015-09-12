/*
    Laser, a UCI chess engine written in C++11.
    Copyright 2015 Jeffrey An and Michael An

    Laser is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Laser is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Laser.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <algorithm>
#include <iomanip>
#include <iostream>
#include "search.h"
#include "searchspace.h"

using namespace std;

/**
 * @brief Records a bunch of useful statistics from the search,
 * which are printed to std error at the end of the search.
 */
struct SearchStatistics {
    uint64_t nodes;
    uint64_t hashProbes, hashHits, hashScoreCuts;
    uint64_t hashMoveAttempts, hashMoveCuts;
    uint64_t failHighs, firstFailHighs;
    uint64_t qsNodes;
    uint64_t qsFailHighs, qsFirstFailHighs;

    SearchStatistics() {
        reset();
    }

    void reset() {
        nodes = 0;
        hashProbes = hashHits = hashScoreCuts = 0;
        hashMoveAttempts = hashMoveCuts = 0;
        failHighs = firstFailHighs = 0;
        qsNodes = 0;
        qsFailHighs = qsFirstFailHighs = 0;
    }
};

/**
 * @brief Records the PV found by the search.
 */
struct SearchPV {
    int pvLength;
    Move pv[MAX_DEPTH+1];

    SearchPV() {
        pvLength = 0;
    }
};

// Futility pruning margins indexed by depth. If static eval is at least this
// amount below alpha, we skip quiet moves for this position.
const int FUTILITY_MARGIN[5] = {0,
    MAX_POS_SCORE,
    MAX_POS_SCORE + 180,
    MAX_POS_SCORE + 400,
    MAX_POS_SCORE + 700
};

// Reverse futility pruning margins indexed by depth. If static eval is at least
// this amount above beta, we skip searching the position entirely.
const int REVERSE_FUTILITY_MARGIN[4] = {0,
    MAX_POS_SCORE - 15,
    MAX_POS_SCORE + 140,
    MAX_POS_SCORE + 360
};

static Hash transpositionTable(16);
static SearchParameters searchParams;
static SearchStatistics searchStats;

// Used to break out of the search thread if the stop command is given
extern bool isStop;

// Search functions
unsigned int getBestMoveAtDepth(Board *b, MoveList &legalMoves, int depth,
    int &bestScore, SearchPV *pvLine);
int PVS(Board &b, int depth, int alpha, int beta, SearchPV *pvLine);
int quiescence(Board &b, int plies, int alpha, int beta);
int checkQuiescence(Board &b, int plies, int alpha, int beta);

// Search helpers
int probeTT(Board &b, Move &hashed, int depth, int &alpha, int beta, SearchPV *pvLine);
int scoreMate(bool isInCheck, int depth, int alpha, int beta);
double getPercentage(uint64_t numerator, uint64_t denominator);
void printStatistics();

// Other utility functions
Move nextMove(MoveList &moves, ScoreList &scores, unsigned int index);
void changePV(Move best, SearchPV *parent, SearchPV *child);
string retrievePV(SearchPV *pvLine);
void feedPVToTT(Board *b, SearchPV *pvLine, int score);


/**
 * @brief Finds a best move for a position according to the given search parameters.
 * @param mode The search mode: time or depth
 * @param value The time limit if in time mode, or the depth to search
 */
void getBestMove(Board *b, int mode, int value, Move *bestMove) {
    searchParams.reset();
    searchStats.reset();
    searchParams.rootMoveNumber = (uint8_t) (b->getMoveNumber());

    int color = b->getPlayerToMove();
    MoveList legalMoves = b->getAllLegalMoves(color);

    // Special case if we are given a mate/stalemate position
    if (legalMoves.size() <= 0) {
        *bestMove = NULL_MOVE;
        isStop = true;
        cout << "bestmove none" << endl;
        return;
    }

    *bestMove = legalMoves.get(0);
    
    // Set up timing
    searchParams.timeLimit = (mode == TIME)
        ? (uint64_t)(MAX_TIME_FACTOR * value) : MAX_TIME;
    searchParams.startTime = ChessClock::now();
    double timeSoFar = getTimeElapsed(searchParams.startTime);

    // Special case if there is only one legal move: use less search time,
    // only to get a rough PV/score
    if (legalMoves.size() == 1 && mode == TIME) {
        searchParams.timeLimit = min(searchParams.timeLimit / 8, ONE_SECOND);
    }

    int bestScore, bestMoveIndex;
    int rootDepth = 1;
    do {
        // Reset all search parameters (killers, plies, etc)
        searchParams.reset();
        // For recording the PV
        SearchPV pvLine;
        // Get the index of the best move
        bestMoveIndex = getBestMoveAtDepth(b, legalMoves, rootDepth, bestScore, &pvLine);
        if (bestMoveIndex == -1)
            break;
        // Swap the PV to be searched first next iteration
        legalMoves.swap(0, bestMoveIndex);
        *bestMove = legalMoves.get(0);
        

        // Calculate values for printing
        timeSoFar = getTimeElapsed(searchParams.startTime);
        uint64_t nps = (uint64_t) ((double) searchStats.nodes / timeSoFar);
        string pvStr = retrievePV(&pvLine);
        
        // Output info using UCI protocol
        cout << "info depth " << rootDepth << " score";

        // Print score in mate or centipawns
        if (bestScore >= MATE_SCORE - MAX_DEPTH)
            // If it is our mate, it takes plies / 2 + 1 moves to mate since
            // our move ends the game
            cout << " mate " << (MATE_SCORE - bestScore) / 2 + 1;
        else if (bestScore <= -MATE_SCORE + MAX_DEPTH)
            // If we are being mated, it takes plies / 2 moves since our
            // opponent's move ends the game
            cout << " mate " << (-MATE_SCORE - bestScore) / 2;
        else
            // Scale score into centipawns using our internal pawn value
            cout << " cp " << bestScore * 100 / PAWN_VALUE_EG;

        cout << " time " << (int)(timeSoFar * ONE_SECOND)
             << " nodes " << searchStats.nodes << " nps " << nps
             << " hashfull " << 1000 * transpositionTable.keys / transpositionTable.getSize()
             << " pv " << pvStr << endl;

        //if (!isStop)
        //    feedPVToTT(b, &pvLine, bestScore);
        // Aging for the history heuristic table
        searchParams.ageHistoryTable(rootDepth);
        rootDepth++;
    }
    while ((mode == TIME  && (timeSoFar * ONE_SECOND < value * TIME_FACTOR)
                          && (rootDepth <= MAX_DEPTH))
        || (mode == DEPTH && rootDepth <= value));
    
    printStatistics();
    // Aging for the history heuristic table
    searchParams.ageHistoryTable(rootDepth);
    
    // Output best move to UCI interface
    isStop = true;
    cout << "bestmove " << moveToString(*bestMove) << endl;
    return;
}

/**
 * @brief Returns the index of the best move in legalMoves.
 */
unsigned int getBestMoveAtDepth(Board *b, MoveList &legalMoves, int depth,
        int &bestScore, SearchPV *pvLine) {
    SearchPV line;
    int color = b->getPlayerToMove();
    unsigned int tempMove = -1;
    int score = -MATE_SCORE;
    int alpha = -MATE_SCORE;
    int beta = MATE_SCORE;
    
    for (unsigned int i = 0; i < legalMoves.size(); i++) {
        Board copy = b->staticCopy();
        copy.doMove(legalMoves.get(i), color);
        searchStats.nodes++;
        
        if (i != 0) {
            searchParams.ply++;
            score = -PVS(copy, depth-1, -alpha-1, -alpha, &line);
            searchParams.ply--;
            if (alpha < score && score < beta) {
                searchParams.ply++;
                score = -PVS(copy, depth-1, -beta, -alpha, &line);
                searchParams.ply--;
            }
        }
        else {
            searchParams.ply++;
            score = -PVS(copy, depth-1, -beta, -alpha, &line);
            searchParams.ply--;
        }

        // Stop condition. If stopping, return search results from incomplete
        // search, if any.
        if (isStop)
            return tempMove;

        if (score > alpha) {
            alpha = score;
            bestScore = score;
            tempMove = i;
            changePV(legalMoves.get(i), pvLine, &line);
        }
    }

    return tempMove;
}

// Gets a best move to try first when a hash move is not available.
int getBestMoveForSort(Board *b, MoveList &legalMoves, int depth) {
    SearchPV line;
    int color = b->getPlayerToMove();
    int tempMove = -1;
    int score = -MATE_SCORE;
    int alpha = -MATE_SCORE;
    int beta = MATE_SCORE;
    
    for (unsigned int i = 0; i < legalMoves.size(); i++) {
        Board copy = b->staticCopy();
        if(!copy.doPseudoLegalMove(legalMoves.get(i), color))
            continue;
        
        if (i != 0) {
            searchParams.ply++;
            score = -PVS(copy, depth-1, -alpha-1, -alpha, &line);
            searchParams.ply--;
            if (alpha < score && score < beta) {
                searchParams.ply++;
                score = -PVS(copy, depth-1, -beta, -alpha, &line);
                searchParams.ply--;
            }
        }
        else {
            searchParams.ply++;
            score = -PVS(copy, depth-1, -beta, -alpha, &line);
            searchParams.ply--;
        }
        
        if (score > alpha) {
            alpha = score;
            tempMove = i;
        }
    }

    return tempMove;
}

//------------------------------------------------------------------------------
//------------------------------Search functions--------------------------------
//------------------------------------------------------------------------------

// The standard implementation of a null-window PVS search.
// The implementation is fail-hard (score returned must be within [alpha, beta])
int PVS(Board &b, int depth, int alpha, int beta, SearchPV *pvLine) {
    // When the standard search is done, enter quiescence search.
    // Static board evaluation is done there.
    if (depth <= 0) {
        pvLine->pvLength = 0;
        return quiescence(b, 0, alpha, beta);
    }

    if (b.isDraw()) {
        if (0 >= beta)
            return beta;
        if (0 > alpha)
            return 0;
        else
            return alpha;
    }
    
    int prevAlpha = alpha;
    int color = b.getPlayerToMove();


    // Probe the hash table for a match/cutoff
    // If a cutoff or exact score hit occurred, probeTT will return a value
    // other than -INFTY
    // alpha is passed by reference in case a hash move raises alpha but does
    // not cause a cutoff
    Move hashed = NULL_MOVE;
    searchStats.hashProbes++;
    int hashScore = probeTT(b, hashed, depth, alpha, beta, pvLine);
    if (hashScore != -INFTY)
        return hashScore;


    SearchPV line;
    PieceMoveList pml = b.getPieceMoveList(color);
    // For PVS, the node is a PV node if beta - alpha > 1 (i.e. not a null window)
    // We do not want to do most pruning techniques on PV nodes
    bool isPVNode = (beta - alpha != 1);
    // Similarly, we do not want to prune if we are in check
    bool isInCheck = b.isInCheck(color);
    // A static evaluation, used to activate null move pruning and futility
    // pruning
    int staticEval = isInCheck ? INFTY
                               : (color == WHITE) ? b.evaluate(pml)
                                                  : -b.evaluate(pml);
    

    // Reverse futility pruning
    // If we are already doing really well and it's our turn, our opponent
    // probably wouldn't have let us get here (a form of the null-move observation
    // adapted to low depths)
    if (!isPVNode && !isInCheck
     && (depth <= 3 && staticEval - REVERSE_FUTILITY_MARGIN[depth] >= beta)
     && b.getNonPawnMaterial(color))
        return beta;


    // Razoring
    // If static eval is a good amount below alpha, we are probably at an all-node.
    // Do a qsearch just to confirm. If the qsearch fails high, a capture gained back
    // the material and trust its result since a quiet move probably can't gain
    // as much.
    if (!isPVNode && !isInCheck
     && ((depth == 1 && staticEval <= alpha - 400)
      || (depth == 2 && staticEval <= alpha - 600)
      || (depth == 3 && staticEval <= alpha - 800))) {
        if (depth == 1)
            return quiescence(b, 0, alpha, beta);

        int value = quiescence(b, 0, alpha, beta);
        if (value <= alpha)
            return alpha;
    }


    // Null move pruning
    // If we are in a position good enough that even after passing and giving
    // our opponent a free turn, we still exceed beta, then simply return beta
    // Only if doing a null move does not leave player in check
    // Do not do if the side to move has only pawns
    // Do not do more than 2 null moves in a row
    if (!isPVNode && !isInCheck
     && depth >= 3 && staticEval >= beta
     && searchParams.nullMoveCount < 2
     && b.getNonPawnMaterial(color)) {
        int reduction;
        // Reduce more if we are further ahead, but do not let NMR descend
        // directly into q-search
        reduction = min(depth - 2,
            1 + (int) ((depth + 2.0) / 5.0 + (double) (staticEval - beta) / 118.0));

        b.doNullMove();
        searchParams.nullMoveCount++;
        searchParams.ply++;
        int nullScore = -PVS(b, depth-1-reduction, -beta, -alpha, &line);
        searchParams.ply--;

        // Undo the null move
        b.doNullMove();
        searchParams.nullMoveCount--;

        if (nullScore >= beta) {
            return beta;
        }
    }


    SearchSpace ss(&b, color, depth, isPVNode, isInCheck, &searchParams);
    // Generate and sort all pseudo-legal moves
    ss.generateMoves(hashed, pml);


    // Main search loop
    // Keeps track of the best move for storing into the TT
    Move toHash = NULL_MOVE;
    // separate counter only incremented when valid move is searched
    unsigned int movesSearched = (hashed == NULL_MOVE) ? 0 : 1;
    int score = -INFTY;
    for (Move m = ss.nextMove(); m != NULL_MOVE;
              m = ss.nextMove()) {
        // Check for a timeout
        double timeSoFar = getTimeElapsed(searchParams.startTime);
        if (timeSoFar * ONE_SECOND > searchParams.timeLimit)
            isStop = true;
        // Stop condition to help break out as quickly as possible
        if (isStop)
            return INFTY;


        bool moveIsPrunable = ss.nodeIsReducible()
                           && !isPromotion(m)
                           && !b.isCheckMove(m, color);


        // Futility pruning
        // If we are already a decent amount of material below alpha, a quiet
        // move probably won't raise our prospects much, so don't bother
        // q-searching it.
        // TODO may fail low in some stalemate cases
        if (moveIsPrunable
         && depth <= 4 && staticEval <= alpha - FUTILITY_MARGIN[depth]
         && !isCapture(m) && abs(alpha) < QUEEN_VALUE) {
            score = alpha;
            continue;
        }


        // Futility pruning using SEE
        /*
        if(moveIsPrunable
        && depth == 1 && staticEval <= alpha
        && abs(alpha) < QUEEN_VALUE
        && ((!isCapture(m) && b.getSEEForMove(color, m) < 0)
         || (isCapture(m) && b.getExchangeScore(color, m) < 0 && b.getSEEForMove(color, m) < -200))) {
            score = alpha;
            continue;
        }
        */


        // Move count based pruning / Late move pruning
        // At low depths, moves late in the list with poor history are pruned
        // As used in Fruit/Stockfish:
        // https://chessprogramming.wikispaces.com/Futility+Pruning#MoveCountBasedPruning
        if (moveIsPrunable
         && ((depth == 1 && movesSearched > 6)
          || (depth == 2 && movesSearched > 12)
          || (depth == 3 && movesSearched > 24))
         && alpha <= prevAlpha && !isCapture(m)
         && m != searchParams.killers[searchParams.ply][0]
         && m != searchParams.killers[searchParams.ply][1]
         && searchParams.historyTable[color][b.getPieceOnSquare(color, getStartSq(m))][getEndSq(m)] < (1 - depth*depth)
            ) {
            score = alpha;
            continue;
        }


        Board copy = b.staticCopy();
        if (!copy.doPseudoLegalMove(m, color))
            continue;
        searchStats.nodes++;


        int reduction = 0;
        // Late move reduction
        // If we have not raised alpha in the first few moves, we are probably
        // at an all-node. The later moves are likely worse so we search them
        // to a shallower depth.
        if (ss.nodeIsReducible()
         && depth >= 3 && movesSearched > 2 && alpha <= prevAlpha
         && !isCapture(m) && !isPromotion(m)
         && m != searchParams.killers[searchParams.ply][0]
         && m != searchParams.killers[searchParams.ply][1]
         && !copy.isInCheck(color^1)) {
            // Increase reduction with higher depth and later moves
            reduction = 1 + (int) ((depth - 4.0) / 5.0 + movesSearched / 16.0);
            // Reduce more for moves with poor history
            int historyValue = searchParams.historyTable[color][b.getPieceOnSquare(color, getStartSq(m))][getEndSq(m)];
            if (historyValue < 0)
                reduction++;

            // Do not let search descend directly into q-search
            reduction = min(reduction, depth - 2);
            // Always start from a reduction of 1 and increase at most 1 depth
            // every 2 moves
            reduction = min(reduction, 1 + (int) (movesSearched - 3) / 2);
        }


        // Null-window search, with re-search if applicable
        if (movesSearched != 0) {
            searchParams.ply++;
            score = -PVS(copy, depth-1-reduction, -alpha-1, -alpha, &line);
            searchParams.ply--;
            // LMR re-search if the reduced search did not fail low
            if (reduction > 0 && score > alpha) {
                searchParams.ply++;
                score = -PVS(copy, depth-1, -alpha-1, -alpha, &line);
                searchParams.ply--;
            }
            // Re-search for a scout window at PV nodes
            else if (alpha < score && score < beta) {
                searchParams.ply++;
                score = -PVS(copy, depth-1, -beta, -alpha, &line);
                searchParams.ply--;
            }
        }
        // The first move is always searched at a normal depth
        else {
            searchParams.ply++;
            score = -PVS(copy, depth-1, -beta, -alpha, &line);
            searchParams.ply--;
        }

        // Stop condition to help break out as quickly as possible
        if (isStop)
            return INFTY;
        
        // Beta cutoff
        if (score >= beta) {
            searchStats.failHighs++;
            if (movesSearched == 0)
                searchStats.firstFailHighs++;
            // Hash moves that caused a beta cutoff
            transpositionTable.add(b, depth, m, beta, CUT_NODE, searchParams.rootMoveNumber);
            // Record killer if applicable
            if (!isCapture(m)) {
                // Ensure the same killer does not fill both slots
                if (m != searchParams.killers[searchParams.ply][0]) {
                    searchParams.killers[searchParams.ply][1] = searchParams.killers[searchParams.ply][0];
                    searchParams.killers[searchParams.ply][0] = m;
                }
                // Update the history table
                searchParams.historyTable[color][b.getPieceOnSquare(color, getStartSq(m))][getEndSq(m)]
                    += depth * depth;
                ss.reduceBadHistories(m);
            }
            return beta;
        }
        // If alpha was raised, we have a new PV
        if (score > alpha) {
            alpha = score;
            toHash = m;
            changePV(m, pvLine, &line);
        }

        movesSearched++;
    }

    // If there were no legal moves
    if (score == -INFTY && movesSearched == 0)
        return scoreMate(ss.isInCheck, depth, alpha, beta);
    
    // Exact scores indicate a principal variation
    if (prevAlpha < alpha && alpha < beta) {
        // If the score wasn't from a move we searched in the main loop, the
        // alpha raise must have come from the hash move
        if (toHash == NULL_MOVE)
            toHash = hashed;
        transpositionTable.add(b, depth, toHash, alpha, PV_NODE, searchParams.rootMoveNumber);
        // Update the history table
        if (!isCapture(toHash)) {
            searchParams.historyTable[color][b.getPieceOnSquare(color, getStartSq(toHash))][getEndSq(toHash)]
                += depth * depth;
            ss.reduceBadHistories(toHash);
        }
    }
    // Record all-nodes. No best move can be recorded in a fail-hard framework.
    else if (alpha <= prevAlpha) {
        transpositionTable.add(b, depth, NULL_MOVE, alpha, ALL_NODE, searchParams.rootMoveNumber);
    }

    return alpha;
}

// See if a hash move exists.
int probeTT(Board &b, Move &hashed, int depth, int &alpha, int beta, SearchPV *pvLine) {
    bool usePVScore = (alpha == beta - 1);
    HashEntry *entry = transpositionTable.get(b);
    if (entry != NULL) {
        searchStats.hashHits++;
        // If the node is a predicted all node and score <= alpha, return alpha
        // since score is an upper bound
        // Vulnerable to Type-1 errors
        int hashScore = entry->score;
        uint8_t nodeType = entry->getNodeType();
        if (nodeType == ALL_NODE) {
            if (entry->depth >= depth && hashScore <= alpha) {
                searchStats.hashScoreCuts++;
                return alpha;
            }
        }
        else {
            hashed = entry->m;
            // Only used a hashed score if the search depth was at least
            // the current depth
            if (entry->depth >= depth) {
                // At cut nodes if hash score >= beta return beta since hash
                // score is a lower bound.
                if (nodeType == CUT_NODE && hashScore >= beta) {
                    searchStats.hashScoreCuts++;
                    searchStats.failHighs++;
                    searchStats.firstFailHighs++;
                    return beta;
                }
                // At PV nodes we can simply return the exact score
                // Do this only at non-PV nodes: at PV nodes we want to ensure
                // a full PV line is returned
                else if (nodeType == PV_NODE && usePVScore) {
                    searchStats.hashScoreCuts++;
                    return hashScore;
                }
            }
            Board copy = b.staticCopy();
            // Sanity check in case of Type-1 hash error
            if (copy.doHashMove(hashed, b.getPlayerToMove())) {
                SearchPV line;
                // If the hash score is unusable and node is not a predicted
                // all-node, we can search the hash move first.
                searchStats.hashMoveAttempts++;
                searchStats.nodes++;
                searchParams.ply++;
                int score = -PVS(copy, depth-1, -beta, -alpha, &line);
                searchParams.ply--;

                if (score >= beta) {
                    searchStats.hashMoveCuts++;
                    return beta;
                }
                if (score > alpha) {
                    alpha = score;
                    changePV(hashed, pvLine, &line);
                }
            }
            else {
                cerr << "Type-1 TT error on " << moveToString(hashed) << endl;
                hashed = NULL_MOVE;
            }
        }
    }
    return -INFTY;
}

// Used to get a score when we have realized that we have no legal moves.
int scoreMate(bool isInCheck, int depth, int alpha, int beta) {
    int score;
    // If we are in check, then checkmate
    if (isInCheck) {
        // Adjust score so that quicker mates are better
        score = (-MATE_SCORE + searchParams.ply);
    }
    else { // else, it is a stalemate
        score = 0;
    }
    if (score >= beta)
        return beta;
    if (score > alpha)
        alpha = score;
    return alpha;
}

/* Quiescence search, which completes all capture and check lines (thus reaching
 * a "quiet" position.)
 * This diminishes the horizon effect and greatly improves playing strength.
 * Delta pruning and static-exchange evaluation are used to reduce the time
 * spent here.
 * The search is done within a fail-hard framework.
 */
int quiescence(Board &b, int plies, int alpha, int beta) {
    int color = b.getPlayerToMove();
    // If in check, we must consider all legal check evasions
    if (b.isInCheck(color))
        return checkQuiescence(b, plies, alpha, beta);

    if (b.isInsufficientMaterial()) {
        if (0 >= beta)
            return beta;
        if (0 > alpha)
            return 0;
        else
            return alpha;
    }

    // Stand pat: if our current position is already way too good or way too bad
    // we can simply stop the search here.
    PieceMoveList pml = b.getPieceMoveList(color);
    int standPat = (color == WHITE) ? b.evaluate(pml) : -b.evaluate(pml);
    
    if (standPat >= beta)
        return beta;

    if (alpha < standPat)
        alpha = standPat;
    
    if (standPat < alpha - MAX_POS_SCORE - QUEEN_VALUE)
        return alpha;


    // Generate captures and order by MVV/LVA
    MoveList legalCaptures = b.getPseudoLegalCaptures(color, pml, false);
    ScoreList scores;
    for (unsigned int i = 0; i < legalCaptures.size(); i++) {
        scores.add(b.getMVVLVAScore(color, legalCaptures.get(i)));
    }
    
    int score = -INFTY;
    unsigned int i = 0;
    unsigned int j = 0; // separate counter only incremented when valid move is searched
    for (Move m = nextMove(legalCaptures, scores, i); m != NULL_MOVE;
              m = nextMove(legalCaptures, scores, ++i)) {
        // Delta prune
        if (standPat + b.valueOfPiece(b.getPieceOnSquare(color^1, getEndSq(m))) < alpha - MAX_POS_SCORE)
            continue;
        // Static exchange evaluation pruning
        if (b.getExchangeScore(color, m) < 0 && b.getSEEForMove(color, m) < -MAX_POS_SCORE)
            continue;
        

        Board copy = b.staticCopy();
        if (!copy.doPseudoLegalMove(m, color))
            continue;
        
        searchStats.nodes++;
        searchStats.qsNodes++;
        score = -quiescence(copy, plies+1, -beta, -alpha);
        
        if (score >= beta) {
            searchStats.qsFailHighs++;
            if (j == 0)
                searchStats.qsFirstFailHighs++;
            return beta;
        }
        if (score > alpha)
            alpha = score;
        j++;
    }

    // Generate and search promotions
    MoveList legalPromotions = b.getPseudoLegalPromotions(color);
    for (unsigned int i = 0; i < legalPromotions.size(); i++) {
        Move m = legalPromotions.get(i);

        // Static exchange evaluation pruning
        if (b.getSEEForMove(color, m) < 0)
            continue;

        Board copy = b.staticCopy();
        if (!copy.doPseudoLegalMove(m, color))
            continue;
        
        searchStats.nodes++;
        searchStats.qsNodes++;
        score = -quiescence(copy, plies+1, -beta, -alpha);
        
        if (score >= beta) {
            searchStats.qsFailHighs++;
            if (j == 0)
                searchStats.qsFirstFailHighs++;
            return beta;
        }
        if (score > alpha)
            alpha = score;
        j++;
    }

    // Checks: only on the first three plies of q-search
    if (plies <= 2) {
        MoveList legalMoves = b.getPseudoLegalChecks(color);

        for (unsigned int i = 0; i < legalMoves.size(); i++) {
            Move m = legalMoves.get(i);

            // Static exchange evaluation pruning
            if (b.getSEEForMove(color, m) < 0)
                continue;

            Board copy = b.staticCopy();
            if (!copy.doPseudoLegalMove(m, color))
                continue;
            
            searchStats.nodes++;
            searchStats.qsNodes++;
            int score = -checkQuiescence(copy, plies+1, -beta, -alpha);
            
            if (score >= beta) {
                searchStats.qsFailHighs++;
                if (j == 0)
                    searchStats.qsFirstFailHighs++;
                alpha = beta;
                return beta;
            }
            if (score > alpha)
                alpha = score;
            j++;
        }
    }

    // TODO This is too slow to be effective
/*    if (score == -INFTY) {
        if (b.isStalemate(color))
            return 0;
    }*/

    return alpha;
}

/*
 * When checks are considered in quiescence, the responses must include all moves,
 * not just captures, necessitating this function.
 */
int checkQuiescence(Board &b, int plies, int alpha, int beta) {
    int color = b.getPlayerToMove();
    PieceMoveList pml = b.getPieceMoveList(color);
    MoveList legalMoves = b.getPseudoLegalCheckEscapes(color, pml);

    int score = -INFTY;
    unsigned int j = 0; // separate counter only incremented when valid move is searched
    for (unsigned int i = 0; i < legalMoves.size(); i++) {
        Move m = legalMoves.get(i);

        if (score != -INFTY && b.getSEEForMove(color, m) < 0)
            continue;

        Board copy = b.staticCopy();
        if (!copy.doPseudoLegalMove(m, color))
            continue;
        
        searchStats.nodes++;
        searchStats.qsNodes++;
        score = -quiescence(copy, plies+1, -beta, -alpha);
        
        if (score >= beta) {
            searchStats.qsFailHighs++;
            if (j == 0)
                searchStats.qsFirstFailHighs++;
            return beta;
        }
        if (score > alpha)
            alpha = score;
        j++;
    }

    // If there were no legal moves
    if (score == -INFTY) {
        // We already know we are in check, so it must be a checkmate
        // Adjust score so that quicker mates are better
        score = (-MATE_SCORE + searchParams.ply + plies);
        if (score >= beta)
            return beta;
        if (score > alpha)
            alpha = score;
    }
    
    return alpha;
}

//------------------------------------------------------------------------------
//------------------------------Other functions---------------------------------
//------------------------------------------------------------------------------

// These functions help to communicate with uci.cpp
void clearTables() {
    transpositionTable.clear();
    searchParams.resetHistoryTable();
}

void setHashSize(uint64_t MB) {
    transpositionTable.setSize(MB);
}

uint64_t getNodes() {
    return searchStats.nodes;
}

// Retrieves the next move with the highest score, starting from index using a
// partial selection sort. This way, the entire list does not have to be sorted
// if an early cutoff occurs.
Move nextMove(MoveList &moves, ScoreList &scores, unsigned int index) {
    if (index >= moves.size())
        return NULL_MOVE;
    // Find the index of the next best move
    int bestIndex = index;
    int bestScore = scores.get(index);
    for (unsigned int i = index + 1; i < moves.size(); i++) {
        if (scores.get(i) > bestScore) {
            bestIndex = i;
            bestScore = scores.get(bestIndex);
        }
    }
    // Swap the best move to the correct position
    moves.swap(bestIndex, index);
    scores.swap(bestIndex, index);
    return moves.get(index);
}

// Copies the new PV line when alpha is raised
void changePV(Move best, SearchPV *parent, SearchPV *child) {
    parent->pv[0] = best;
    for (int i = 0; i < child->pvLength; i++) {
        parent->pv[i+1] = child->pv[i];
    }
    parent->pvLength = child->pvLength + 1;
}

// Recover PV for outputting to terminal / GUI using transposition table entries
string retrievePV(SearchPV *pvLine) {
    string pvStr = moveToString(pvLine->pv[0]);
    for (int i = 1; i < pvLine->pvLength; i++) {
        pvStr += " " + moveToString(pvLine->pv[i]);
    }

    return pvStr;
}

// Feeds the PV to the transposition table so that it will be searched first
// next time
void feedPVToTT(Board *b, SearchPV *pvLine, int score) {
    if (pvLine->pvLength <= 2)
        return;
    int color = b->getPlayerToMove();
    Board copy = b->staticCopy();
    copy.doMove(pvLine->pv[0], color);
    copy.doMove(pvLine->pv[1], color^1);

    for (int i = 2; i < pvLine->pvLength; i++) {
        transpositionTable.addPV(copy, pvLine->pvLength - i, pvLine->pv[i], score, searchParams.rootMoveNumber);
        copy.doMove(pvLine->pv[i], color);
        color = color ^ 1;
        score = -score;
    }
}

// Formats a fraction into a percentage value (0 to 100) for printing
double getPercentage(uint64_t numerator, uint64_t denominator) {
    if (denominator == 0)
        return 0;
    uint64_t tenThousandths = (numerator * 10000) / denominator;
    double percent = ((double) tenThousandths) / 100.0;
    return percent;
}

// Prints the statistics gathered during search
void printStatistics() {
    cerr << setw(22) << "Hash hitrate: " << getPercentage(searchStats.hashHits, searchStats.hashProbes)
         << '%' << " of " << searchStats.hashProbes << " probes" << endl;
    cerr << setw(22) << "Hash score cut rate: " << getPercentage(searchStats.hashScoreCuts, searchStats.hashHits)
         << '%' << " of " << searchStats.hashHits << " hash hits" << endl;
    cerr << setw(22) << "Hash move cut rate: " << getPercentage(searchStats.hashMoveCuts, searchStats.hashMoveAttempts)
         << '%' << " of " << searchStats.hashMoveAttempts << " hash moves" << endl;
    cerr << setw(22) << "First fail high rate: " << getPercentage(searchStats.firstFailHighs, searchStats.failHighs)
         << '%' << " of " << searchStats.failHighs << " fail highs" << endl;
    cerr << setw(22) << "QS Nodes: " << searchStats.qsNodes << " ("
         << getPercentage(searchStats.qsNodes, searchStats.nodes) << '%' << ")" << endl;
    cerr << setw(22) << "QS FFH rate: " << getPercentage(searchStats.qsFirstFailHighs, searchStats.qsFailHighs)
         << '%' << " of " << searchStats.qsFailHighs << " qs fail highs" << endl;
}