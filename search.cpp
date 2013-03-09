#include "scorpio.h"

static const int CHECK_DEPTH = UNITDEPTH;

static int use_iid = 1;
static int futility_margin = 125;
static int use_singular = 0;
static int singular_margin = 30;

#define extend(value) {                      \
	extension += value;                      \
	pstack->extension = 1;                   \
}

#define reduce(value) {                      \
	pstack->depth -= value;                  \
	pstack->reduction += value;              \
}

/*
* Update pv
*/
FORCEINLINE void SEARCHER::UPDATE_PV(MOVE move)  {
	pstack->pv[ply] = move; 
	memcpy(&pstack->pv[ply + 1] , &(pstack + 1)->pv[ply + 1] , ((pstack + 1)->pv_length - ply) * sizeof(MOVE));
	pstack->pv_length = (pstack + 1)->pv_length;
}

bool SEARCHER::hash_cutoff() { 
	/*
	. hashtable lookup 
	*/
	pstack->hash_flags = probe_hash(player,hash_key,pstack->depth,ply,pstack->hash_score,
		pstack->hash_move,pstack->alpha,pstack->beta,pstack->mate_threat,pstack->hash_depth);
	if(pstack->hash_move && !is_legal_fast(pstack->hash_move))
		pstack->hash_move = 0;
	if(pstack->node_type != PV_NODE && pstack->hash_flags >= EXACT) {
		if(pstack->hash_move) {
			pstack->pv_length = ply + 1;
			pstack->pv[ply] = pstack->hash_move;
		}
		pstack->best_move = pstack->hash_move;
		pstack->best_score = pstack->hash_score;
		return true;
	}
	return false;
}
bool SEARCHER::bitbase_cutoff() {

#ifdef EGBB
	/*
	. Cutoff tree only if we think "progress" is being made
	. after captures
	. after pawn moves
	. or just after a certain ply (probe_depth) 
	*/
	if( egbb_is_loaded
		&& all_man_c <= 5
		&& (ply >= probe_depth
		|| is_cap_prom((pstack - 1)->current_move)
		|| PIECE(m_piece((pstack - 1)->current_move)) == pawn
		)
		) {
			/*
			Probe bitbases at leafs ,only if they are loaded in RAM
			*/
			register int score;
			if((ply <= probe_depth 
				|| (egbb_load_type >= 1 && all_man_c <= 4)
				|| egbb_load_type == 3)
				&& probe_bitbases(score)
				) {

					egbb_probes++;

					/*prefer wins near root*/
					if(score > 0)
						score -= WIN_PLY * (ply + 1);
					else if(score < 0)
						score += WIN_PLY * (ply + 1);

					pstack->best_score = score;

					return true;
			}
	}
#endif
	/*
	. no cutoff
	*/
	return false;
}
FORCEINLINE int SEARCHER::on_node_entry() {

	/*qsearch?*/
	if(pstack->depth <= 0) {
		prefetch_qtt();
		if(pstack->node_type == PV_NODE) pstack->qcheck_depth = 2 * CHECK_DEPTH;	
		else pstack->qcheck_depth = CHECK_DEPTH;
		qsearch();
		return true;
	}
	prefetch_tt();

	/*razoring & static pruning*/
	if(pstack->depth <= 4 * UNITDEPTH
		&& (pstack - 1)->search_state != NULL_MOVE
		&& !pstack->extension
		&& pstack->node_type != PV_NODE
		) {
			int score = eval();
			int margin = futility_margin + 
				50 * (DEPTH(pstack->depth) - 1) * (DEPTH(pstack->depth) - 1);
			if(score + margin < pstack->alpha) {
				pstack->qcheck_depth = CHECK_DEPTH;
				qsearch();
				score = pstack->best_score;
				if(score + margin < pstack->alpha)
					return true;
			} else if(score - margin >= pstack->beta) {
				pstack->best_score = pstack->beta;
				return true;
			}
	}

    /*initialize node*/
	nodes++;

	pstack->gen_status = GEN_START;
	pstack->flag = UPPER;
	pstack->pv_length = ply;
	pstack->legal_moves = 0;
	pstack->hash_move = 0;
	pstack->best_score = -MATE_SCORE;
	pstack->best_move = 0;
	pstack->mate_threat = 0;
	pstack->singular = 0;

	if(pstack->node_type == PV_NODE) {
		pstack->next_node_type = PV_NODE;
	} else if(pstack->node_type == ALL_NODE) {
		pstack->next_node_type = CUT_NODE;
	} else {
		pstack->next_node_type = ALL_NODE;
	}

	if(pstack->alpha > MATE_SCORE - WIN_PLY * (ply)) {
		pstack->best_score = pstack->alpha;
		return true; 
	}

	if(draw()) {
		pstack->best_score = 0;
		return true;
	}

	if(ply >= MAX_PLY - 1) {
		pstack->best_score = eval();
		return true;
	}

	/*
	* Processor 0 of host 0 does polling for input
	* from keypress and checks for time limit.
	*/
	if(processor_id == 0 CLUSTER_CODE(&& PROCESSOR::host_id == 0)) {
		if(nodes > time_check) {
			time_check += poll_nodes;
			if(!abort_search)
				check_quit();
			if(abort_search) {
				CLUSTER_CODE(PROCESSOR::quit_hosts());
				return true;
			}
		} 
	}

	/*check for messages from other hosts*/
#ifdef CLUSTER
#	ifndef THREAD_POLLING
	if(processor_id == 0 && nodes > message_check) {
		processors[processor_id]->idle_loop();
		message_check += PROCESSOR::MESSAGE_POLL_NODES;
	}
#	else
	if(processor_id == 0 && PROCESSOR::message_available) {
		processors[processor_id]->idle_loop();
	}
#	endif
#endif   

	/*probe hash table*/
	if(hash_cutoff())
		return true;

	/*bitbase cutoff*/
	if(bitbase_cutoff())
		return true;

	return false;
}

FORCEINLINE int SEARCHER::on_qnode_entry() {
	int score;

	/*initialize node*/
	nodes++;
	qnodes++;

	pstack->gen_status = GEN_START;
	pstack->pv_length = ply;
	pstack->best_score = -MATE_SCORE;
	pstack->best_move = 0;
	pstack->legal_moves = 0;

	if(pstack->alpha > MATE_SCORE - WIN_PLY * ply) {
		pstack->best_score = pstack->alpha;
		return true; 
	}

	if(draw()) {
		pstack->best_score = 0;
		return true;
	}

	if(ply >= MAX_PLY - 1) {
		pstack->best_score = eval();
		return true;
	}

	/*bitbase cutoff*/
	if(bitbase_cutoff())
		return true;

	/*stand pat*/
	if(!hstack[hply - 1].checks) {
		score = eval();
		pstack->best_score = score;
		if(score > pstack->alpha) {
			if(score >= pstack->beta)
				return true;
			pstack->alpha = score;
		}
	}

	return false;
}
/*passed pawn moves*/
int SEARCHER::is_passed(MOVE move,int type) const {
	if(PIECE(m_piece(move)) != pawn)
		return 0;
	int to = m_to(move),f = file(to),r = rank(to),sq;
	if(opponent == white) {
		if(r == RANK7) return 2;
		if(type == LASTR) return 0;
		if(type == HALFR && r <= RANK4) return 0;
		if(r == RANK8) return 0;
		for(sq = to + UU;sq < A8 + f;sq += UU) {
			if(board[sq] == bpawn || board[sq + RR] == bpawn || board[sq + LL] == bpawn)
				return 0;
		}
	} else {
		if(r == RANK2) return 2;
		if(type == LASTR) return 0;
		if(type == HALFR && r >= RANK5) return 0;
		if(r == RANK1) return 0;
		for(sq = to + DD;sq > A1 + f;sq += DD) {
			if(board[sq] == wpawn || board[sq + RR] == wpawn || board[sq + LL] == wpawn)
				return 0;
		}
	}
	return 1;
}
/*selective search*/
int SEARCHER::be_selective() {
	register MOVE move = (pstack - 1)->current_move; 
	register int extension = 0,score,depth = DEPTH((pstack - 1)->depth);
	register int node_t = (pstack - 1)->node_type,nmoves = (pstack - 1)->legal_moves;

	pstack->extension = 0;
	pstack->reduction = 0;
	/*
	extend
	*/
	if(node_t == PV_NODE) {
		if(hstack[hply - 1].checks) {
			extend(UNITDEPTH);
		}
		if(is_passed(move,HALFR)) { 
			extend(UNITDEPTH);
		}
	} else {
		if(hstack[hply - 1].checks) {
			extend(0);
		}
		if(depth >= 6 && is_passed(move,HALFR)) { 
			extend(0);
		}
	}
	if((pstack - 1)->mate_threat) {
		extend(0);
	}
	if((pstack - 1)->count == 1
		&& hstack[hply - 2].checks
		) {
		extend(UNITDEPTH / 2);
	}
	if(m_capture(move)
		&& m_capture(hstack[hply - 2].move)
		&& m_to(move) == m_to(hstack[hply - 2].move)
		&& piece_cv[m_capture(move)] == piece_cv[m_capture(hstack[hply - 2].move)]
	) {
		extend(UNITDEPTH);
	}
	if(m_capture(move)
		&& piece_c[white] + piece_c[black] == 0
		&& PIECE(m_capture(move)) != pawn
		) {
			extend(UNITDEPTH);
	}
	if(nmoves == 1 && (pstack - 1)->singular) {
		extend(UNITDEPTH);
	}
	if(extension > UNITDEPTH)
		extension = UNITDEPTH;

	pstack->depth += extension; 
	/*
	pruning
	*/
	if(depth <= 7
		&& !pstack->extension
		&& (pstack - 1)->gen_status - 1 == GEN_NONCAPS
		&& node_t != PV_NODE
		) {
			if(depth <= 2 && nmoves >= 8)
				return true;

			int margin = futility_margin * depth;
			margin = MAX(margin / 4, margin - 10 * nmoves);
			if(pstack->depth <= 0) 
				margin = 0;

			score = -eval();
			
			if(score + margin < (pstack - 1)->alpha) {
				if(score > (pstack - 1)->best_score) {
					(pstack - 1)->best_score = score;
					(pstack - 1)->best_move = move;
				}
				return true;
			}
	}
	/*
	late move reduction
	*/
	if(!pstack->extension
		&& (pstack - 1)->gen_status - 1 == GEN_NONCAPS
		) {
			if(nmoves >= 2 && pstack->depth > UNITDEPTH) {
				reduce(UNITDEPTH);
				if(nmoves >= ((node_t == PV_NODE) ? 8 : 4) && pstack->depth > UNITDEPTH) {
					reduce(UNITDEPTH);
					if(nmoves >= 16 && pstack->depth >= 4 * UNITDEPTH) {
						reduce(UNITDEPTH);
						if(nmoves >= 24 && pstack->depth >= 4 * UNITDEPTH) {
							reduce(2 * UNITDEPTH);
							if(nmoves >= 32 && pstack->depth >= 4 * UNITDEPTH) {
								reduce(2 * UNITDEPTH);
							}
						}
					}
				}
			}
	}
	/*
	end
	*/
	return false;
}
/*
Back up to previous ply
*/
#define GOBACK(save) {																							\
	if(save && ((sb->pstack->search_state & ~MOVE_MASK) != SINGULAR_SEARCH)) {     	                            \
		sb->record_hash(sb->player,sb->hash_key,sb->pstack->depth,sb->ply,sb->pstack->flag,						\
		sb->pstack->best_score,sb->pstack->best_move,sb->pstack->mate_threat);									\
	}																											\
	if(sb->pstack->search_state & ~MOVE_MASK) goto SPECIAL;                                                     \
	goto POP;                                                                                                   \
};
/*
Iterative search.
*/
#ifdef PARALLEL
void search(PROCESSOR* const proc) {
#else
void search(SEARCHER* const sb) {
#endif

	register MOVE move;
	register int score = 0;
#ifdef PARALLEL
	register PSEARCHER sb = proc->searcher;
	register int active_workers;
	CLUSTER_CODE(register int active_hosts;)
#endif

		/*
		* First processor goes on searching, while the
		* rest go to sleep mode.
		*/
		if(sb SMP_CODE(&& proc->state == GO)) {
			sb->stop_ply = sb->ply;
			goto NEW_NODE;
		} else {
			SMP_CODE(goto IDLE_START;);
		}

		/*
		* Iterative depth first search (DFS)
		*/
		while(true) {
			/*
			* GO forward pushing moves until tip of tree is reached
			*/
			while(true) {
				SMP_CODE (START:);
				/*
				* IID and Singular search. These two are related in a way because
				* we do IID at every node to get a move to be tested for singularity later.
				* Try both at every node other than _ALL_ nodes. Use moderate reduction for IID
				* so that we have a good enough move. In the test for singularity reduce the depth
				* further by 2 plies.
				*/
				switch(sb->pstack->search_state) {
					case IID_SEARCH:
						if(use_iid
							&& sb->pstack->node_type != ALL_NODE
							&& !sb->pstack->hash_move
							&& sb->pstack->depth >= 6 * UNITDEPTH
							) {
								sb->pstack->o_alpha = sb->pstack->alpha;
								sb->pstack->o_beta = sb->pstack->beta;
								sb->pstack->o_depth = sb->pstack->depth;
								if(sb->pstack->node_type == PV_NODE) 
									sb->pstack->depth -= 2 * UNITDEPTH;
								else 
									sb->pstack->depth -= 4 * UNITDEPTH;
								sb->pstack->search_state |= NORMAL_MOVE; 
						} else {
							sb->pstack->search_state = SINGULAR_SEARCH;
						}
						break;
					case SINGULAR_SEARCH:
						if(use_singular 
							&& sb->pstack->node_type != ALL_NODE
							&& sb->pstack->hash_move 
							&& sb->pstack->hash_depth >= 6 * UNITDEPTH
							&& sb->pstack->hash_flags == HASH_GOOD
							) {
								sb->pstack->o_alpha = sb->pstack->alpha;
								sb->pstack->o_beta = sb->pstack->beta;
								sb->pstack->o_depth = sb->pstack->depth;
								sb->pstack->alpha = sb->pstack->hash_score - singular_margin;
								sb->pstack->beta = sb->pstack->alpha + 1;
								sb->pstack->depth = sb->pstack->hash_depth - 2 * UNITDEPTH;
								sb->pstack->search_state |= NORMAL_MOVE; 
						} else {
							sb->pstack->search_state = NORMAL_MOVE;
						}
						break;
				}
				/*
				* Get a legal move (normal/null) and play it on the board.
				* Null move implementation is clearer this way.
				*/
				switch(sb->pstack->search_state & MOVE_MASK) {
					case NULL_MOVE:
						if(!sb->hstack[sb->hply - 1].checks
							&& sb->pstack->hash_flags != AVOID_NULL
							&& sb->pstack->depth >= 2 * UNITDEPTH
							&& sb->pstack->node_type != PV_NODE
							&& sb->piece_c[sb->player]
						&& (score = sb->eval()) >= sb->pstack->beta
							) {
								sb->PUSH_NULL();
								sb->pstack->extension = 0;
								sb->pstack->reduction = 0;
								sb->pstack->alpha = -(sb->pstack - 1)->beta;
								sb->pstack->beta = -(sb->pstack - 1)->beta + 1;
								sb->pstack->node_type = (sb->pstack - 1)->next_node_type;
								/* Smooth scaling from Dann Corbit */
								sb->pstack->depth = (sb->pstack - 1)->depth - 4 * UNITDEPTH;
								if(score >= (sb->pstack - 1)->beta)
									sb->pstack->depth -= (MIN(3 , (score - (sb->pstack - 1)->beta) / 32) * (UNITDEPTH / 2));
								/* Try double NULL MOVE in late endgames to avoid some bad
								* play of KB(N)*K* endings. Idea from Vincent Diepeeven.
								*/
								if(sb->piece_c[sb->player] <= 9 
									&& (sb->ply < 2 || (sb->pstack - 2)->search_state != NULL_MOVE))
									sb->pstack->search_state = NULL_MOVE;
								else
									sb->pstack->search_state = NORMAL_MOVE;
								goto NEW_NODE;
						}
						sb->pstack->search_state = IID_SEARCH;
						break;
					case NORMAL_MOVE:
						while(true) {
#ifdef PARALLEL
							/*
							* Get Smp move
							*/
							if(sb->master && sb->stop_ply == sb->ply) {
								if(!sb->get_smp_move())
									GOBACK(false); //I
							} else
#endif
							/*
							* Get a move in the regular manner
							*/
							{
								if(!sb->get_move()) {
									if(!sb->pstack->legal_moves) {
										if(sb->hstack[sb->hply - 1].checks) {
											sb->pstack->best_score = -MATE_SCORE + WIN_PLY * (sb->ply + 1);
										} else {
											sb->pstack->best_score = 0;
										}
										sb->pstack->flag = EXACT;
									}
									GOBACK(true);
								}

								sb->pstack->legal_moves++;
							}
							/*
							* singular search?
							*/
							if(sb->pstack->legal_moves == 1
								&& (sb->pstack->search_state & ~MOVE_MASK) == SINGULAR_SEARCH) {
									continue;
							}
							/*
							* play the move
							*/
							sb->PUSH_MOVE(sb->pstack->current_move);

							/*set next ply's depth and be selective*/			
							sb->pstack->depth = (sb->pstack - 1)->depth - UNITDEPTH;
							if(sb->be_selective()) {
								sb->POP_MOVE();
								continue;
							}
							/*next ply's window*/
							if((sb->pstack - 1)->node_type == PV_NODE && (sb->pstack - 1)->legal_moves > 1) {
								sb->pstack->alpha = -(sb->pstack - 1)->alpha - 1;
								sb->pstack->beta = -(sb->pstack - 1)->alpha;
								sb->pstack->node_type = CUT_NODE;
								sb->pstack->search_state = NULL_MOVE;
							} else {
								sb->pstack->alpha = -(sb->pstack - 1)->beta;
								sb->pstack->beta = -(sb->pstack - 1)->alpha;
								if((sb->pstack - 1)->legal_moves > 3)
									sb->pstack->node_type = CUT_NODE;
								else
									sb->pstack->node_type = (sb->pstack - 1)->next_node_type;
								sb->pstack->search_state = NULL_MOVE;
							}
							/*go to new node*/
							goto NEW_NODE;
						}
				}
				/*
				* At this point we have made a normal/null move and already set up
				* the bounds,depth and other next node states. It is time to check all
				* the conditions which cause cutoff (repetition draws, hashtable/bitbase cutoffs,
				* qsearch(Yes:). Qsearch is implemented separately.
				*/
NEW_NODE:
				if(sb->on_node_entry())
					goto POP;
			}
POP:
			/*
			* Terminate search on current block if stop signal is on
			* or if we finished searching.
			*/
			if(sb->stop_ply == sb->ply 
				|| sb->stop_searcher 
				|| sb->abort_search
				) {
#ifndef PARALLEL
					return;
#else
					/*
					* Is this processor a slave?
					*/
					if(sb->master) {

						l_lock(lock_smp);
						l_lock(sb->master->lock);
						sb->update_master();
						active_workers = sb->master->n_workers;
						CLUSTER_CODE(active_hosts = sb->master->n_host_workers);
						l_unlock(sb->master->lock);
						l_unlock(lock_smp);
						/*
						* When all helper thread workers are idle but the last helper host is not,
						* just wait here! It is difficult to do it otherwise because
						*  i)  Not all threads are idle, since some could be working elsewhere. This
						*      is a direct consequence of using SMP at a node. SMP is good to share TT at a node
						*      and i don't want to change things so that a process is started for each processor.
						*  ii) Other hosts can't continue the current search from here. And it is difficult to do 
						*      synchronized master to slave conversion. A special "HELP if same master" kind
						*      of message could be sent to resolve this but problem (i) still remains unsolved.
						*/ 
#ifdef CLUSTER
						if(!active_workers && active_hosts) {
							while(sb->master->n_host_workers) 
								proc->idle_loop();
							active_hosts = 0;
						}
#endif
						/*
						* We have run out of work now.If this was the _last worker_, grab master 
						* search block and continue searching with it (IE backup).Otherwise goto 
						* idle mode until work is assigned. In a recursive search, only the thread 
						* who initiated the split can backup.
						*/
						if(!active_workers 
							CLUSTER_CODE( && !active_hosts)
							) {
								sb->master->stop_searcher = 0;
								l_lock(lock_smp);
								proc->searcher = sb->master;
								/* Did we get here through the same processor that we splitted with ?
								* If not make sure we have the correct link to the master's master.
								*/
								SEARCHER* sbm = sb->master;
								if(sbm->processor_id != sb->processor_id && sbm->master) {
									sbm->master->workers[sbm->processor_id] = 0; 
									sbm->master->workers[sb->processor_id] = sbm;
								}
								/*end*/
								proc->searcher->processor_id = sb->processor_id;
								sb->used = false;
								sb = proc->searcher;
								l_unlock(lock_smp);

								GOBACK(true);
						} else {
							/*
							* make processor idle
							*/ 
							l_lock(lock_smp);
							sb->used = false;
							if(proc->state == GO) {
								proc->searcher = NULL;
								proc->state = WAIT;
							}
							l_unlock(lock_smp);
IDLE_START:	
							/*
							* processor's state loop
							*/
							while(true) {
								switch(proc->state) {
							case PARK:
							case WAIT:
								l_lock(lock_smp);		
								PROCESSOR::n_idle_processors++;	
								l_unlock(lock_smp);

								proc->idle_loop();

								l_lock(lock_smp);		
								PROCESSOR::n_idle_processors--;	
								l_unlock(lock_smp);

								break;
							case GO: 
								sb = proc->searcher;
								goto START;
							case KILL:
								proc->state = GO;
								return;
								}
							}
						}
						/*
						* This processor is a master. Only processor[0] can return from here to the root!
						* If some other processor reached here first switch to processor[0] and return.
						*/
					} else {
						if(proc == processors[0]) {
							return;
						} else {
							/* Switch to processor[0] and send
							* the current processor to sleep
							*/
							l_lock(lock_smp);
							sb->processor_id = 0;
							processors[0]->searcher = sb;
							processors[0]->state = KILL;
							proc->searcher = NULL;
							proc->state = WAIT;
							l_unlock(lock_smp);

							goto IDLE_START;
						}
					}
#endif
			}
			/*
			* decide to go back one step OR research
			* with a different window/depth
			*/
			score = -sb->pstack->best_score;

			switch((sb->pstack - 1)->search_state & MOVE_MASK) {
			case NULL_MOVE:
				sb->POP_NULL();
				break;
			case NORMAL_MOVE:
				/*research with full depth*/
				if(sb->pstack->reduction > 0
					&& score > (sb->pstack - 1)->alpha
					) {
						sb->pstack->depth += UNITDEPTH;
						sb->pstack->reduction -= UNITDEPTH;
						sb->pstack->alpha = -(sb->pstack - 1)->alpha - 1;
						sb->pstack->beta = -(sb->pstack - 1)->alpha;
						sb->pstack->node_type = CUT_NODE;
						sb->pstack->search_state = NULL_MOVE;
						goto NEW_NODE;
				}
				/*research with full window*/
				if((sb->pstack - 1)->node_type == PV_NODE 
					&& sb->pstack->node_type != PV_NODE
					&& score > (sb->pstack - 1)->alpha
					&& score < (sb->pstack - 1)->beta
					) {
						sb->pstack->alpha = -(sb->pstack - 1)->beta;
						sb->pstack->beta = -(sb->pstack - 1)->alpha;
						sb->pstack->node_type = PV_NODE;
						sb->pstack->search_state = NULL_MOVE;
						goto NEW_NODE;
				}
				sb->POP_MOVE();
				break;
			}
			/*
			* At this point move is taken back after researches,if any,are done.
			* So we can update alpha now.
			*/
			switch(sb->pstack->search_state & MOVE_MASK) {
				case NULL_MOVE:
					if(score >= sb->pstack->beta) { 
						sb->pstack->best_score = score;
						sb->pstack->best_move = 0;
						sb->pstack->flag = LOWER;
						GOBACK(true);
					} else {
						if(score == -MATE_SCORE + WIN_PLY * (sb->ply + 3))
							sb->pstack->mate_threat = 1;
					}
					sb->pstack->search_state = IID_SEARCH;
					break;
				case NORMAL_MOVE:
					/*update bounds*/
					move = sb->pstack->current_move;
					if(score > sb->pstack->best_score) {
						sb->pstack->best_score = score;
						sb->pstack->best_move = move;
					}
					if(score > sb->pstack->alpha) {
						if(score >= sb->pstack->beta) {
							sb->pstack->flag = LOWER;
							if(!is_cap_prom(move))
								sb->update_history(move);
#ifdef PARALLEL		
							/* stop workers*/
							if(sb->master && sb->stop_ply == sb->ply)
								sb->master->handle_fail_high();
#endif
							GOBACK(true);
						}
						sb->pstack->alpha = score;
						sb->pstack->flag = EXACT;
						sb->UPDATE_PV(move);
					}
#ifdef PARALLEL	
					/*
					* Check split here since at least one move is searched
					* at this point. On success reset the sb pointer to the child
					* block pointed to by this processor.
					*/
					if(sb->check_split()) 
						sb = proc->searcher;
#endif
					break;
			}
			/*
			* Go up and process another move.
			*/

			continue;

SPECIAL:
			switch(sb->pstack->search_state & ~MOVE_MASK) {
				case IID_SEARCH:
					sb->pstack->hash_move  = sb->pstack->best_move;
					sb->pstack->hash_score = sb->pstack->best_score;
					sb->pstack->hash_depth = sb->pstack->depth;
					sb->pstack->hash_flags = sb->pstack->flag;
					if(sb->pstack->flag == EXACT || sb->pstack->flag == LOWER)
						sb->pstack->hash_flags = HASH_GOOD;
					sb->pstack->search_state = SINGULAR_SEARCH;
					break;
				case SINGULAR_SEARCH:
					if(sb->pstack->flag == UPPER)
						sb->pstack->singular = 1;
					sb->pstack->search_state = NORMAL_MOVE;
					break;
			} 
			sb->pstack->alpha = sb->pstack->o_alpha;
			sb->pstack->beta = sb->pstack->o_beta;
			sb->pstack->depth = sb->pstack->o_depth;
			sb->pstack->gen_status = GEN_START;
			sb->pstack->flag = UPPER;
			sb->pstack->legal_moves = 0;
			sb->pstack->best_score = -MATE_SCORE;
			sb->pstack->best_move = 0;
			sb->pstack->pv_length = sb->ply;
			/*
			end
			*/
		}
}

/*
searcher's search function
*/
void SEARCHER::search() {
#ifdef PARALLEL
	::search(processors[0]);
#else
	::search(this);
#endif
}
/*
quiescent search
*/
void SEARCHER::qsearch() {

	register int score;
	register int stop_ply = ply;

	goto NEW_NODE_Q;

	while(true) {
		while(true) {

			/*get legal move*/
			if(!get_qmove()) {
				if(hstack[hply - 1].checks && pstack->legal_moves == 0)
					pstack->best_score = -MATE_SCORE + WIN_PLY * (ply + 1);
				goto POP_Q;
			}

			pstack->legal_moves++;

			PUSH_MOVE(pstack->current_move);

			/*next ply's window and depth*/ 
			pstack->alpha = -(pstack - 1)->beta;
			pstack->beta = -(pstack - 1)->alpha;
			pstack->depth = (pstack - 1)->depth - UNITDEPTH;
			pstack->qcheck_depth = (pstack - 1)->qcheck_depth - UNITDEPTH;
			/*end*/

NEW_NODE_Q:		
			/*
			NEW node entry point
			*/
			if(on_qnode_entry())
				goto POP_Q;

		}
POP_Q:
		if(stop_ply == ply)
			return;

		POP_MOVE();
		score = -(pstack + 1)->best_score;

		if(score > pstack->best_score) {
			pstack->best_score = score;
			pstack->best_move = pstack->current_move;

			if(score > pstack->alpha) {
				if(score >= pstack->beta)
					goto POP_Q;
				pstack->alpha = score;
				UPDATE_PV(pstack->current_move);
			}
		}
	}
}

/*
@ root lots of things are done differently so
lets have a separate search function
*/

void SEARCHER::root_search() {

	MOVE move;
	int score;

	pstack->best_score = -MATE_SCORE;
	pstack->best_move = 0;
	pstack->flag = UPPER;
	pstack->hash_move = 0;
	pstack->mate_threat = 0;
	pstack->legal_moves = 0;
	pstack->node_type = PV_NODE;
	pstack->next_node_type = PV_NODE;
	pstack->extension = 0;
	pstack->reduction = 0;

	/*search root moves*/
	UBMP64 start_nodes;
	int& i = pstack->current_index;

	for(i = 0; i < pstack->count;i++) {
		pstack->current_move = move = pstack->move_st[i];
		pstack->legal_moves++;
		PUSH_MOVE(move);

		start_nodes = nodes;
		pstack->extension = 0;
		pstack->reduction = 0;

		/*set next ply's depth*/	
		pstack->depth = (pstack - 1)->depth - UNITDEPTH;
		if(i >= 3
			&& !hstack[hply - 1].checks
			&& !m_capture(move)
			&& !is_passed(move,HALFR)
			) {
				reduce(UNITDEPTH);
		}

		/*search call*/
#define SEARCH(type) {                                     \
	if(type == PV_NODE) {                                  \
		pstack->alpha = -(pstack - 1)->beta;               \
		pstack->beta = -(pstack - 1)->alpha;               \
	} else {                                               \
		pstack->alpha = -(pstack - 1)->alpha - 1;          \
		pstack->beta = -(pstack - 1)->alpha;               \
	}                                                      \
	pstack->node_type = type;                              \
	pstack->search_state = NULL_MOVE;                      \
	search();                                              \
	if(abort_search) return;                               \
	score = -pstack->best_score;                           \
}
		if(i == 0) {
			SEARCH(PV_NODE);
		} else {
			SEARCH(CUT_NODE);
			/*research with full depth*/
			while(pstack->reduction > 0
				&& score > (pstack - 1)->alpha
				) {
					reduce(-UNITDEPTH);
					SEARCH(CUT_NODE);
			}
			/*research*/
			if(score > (pstack - 1)->alpha 
				&& score < (pstack - 1)->beta
				) {
					SEARCH(PV_NODE);
			}
		}

		/*undo move*/
		POP_MOVE();

		root_score_st[i] += (nodes - start_nodes);

		if(i == 0 || score > pstack->alpha) {
			/*update pv*/
			pstack->best_score = score;
			pstack->best_move = move;

			if(score < pstack->beta) {
				UPDATE_PV(move);
			} else {
				pstack->pv[0] = move;
				pstack->pv_length = 1;
				pstack->flag = LOWER;
			}

			/*print pv*/
			if(search_depth >= 5)
				print_pv(pstack->best_score);

			/*root score*/
			if(!chess_clock.infinite_mode && !chess_clock.pondering)
				root_score = score;

			/*fail low*/
			if(score <= pstack->alpha) {
				root_failed_low = 2;
				goto END;
			}
		}
		if(score > pstack->alpha) {
			if(score >= pstack->beta) {
				pstack->flag = LOWER;
				goto END;
			}
			pstack->alpha = score;
			pstack->flag = EXACT;
		}
	}
	print_pv(pstack->best_score);
END:
	record_hash(player,hash_key,pstack->depth,ply,pstack->flag,
		pstack->best_score,pstack->best_move,0);
}

/*
gets best move of position
*/
MOVE SEARCHER::find_best() {
	int legal_moves,i,score,scorer = 0,time_used;
	int easy = false,easy_score = 0;
	int prev_root_score = 0;
	MOVE easy_move = 0;

	ply = 0;
	pstack = stack + 0;
	nodes = 0;
	qnodes = 0;
	time_check = 0;
	message_check = 0;
	splits = 0;
	bad_splits = 0;
	stop_searcher = 0;
	abort_search = 0;
	search_depth = 1;
	poll_nodes = 5000;
	egbb_probes = 0;
	start_time = get_time();
	PROCESSOR::age = (hply & AGE_MASK);
	in_egbb = (egbb_is_loaded && all_man_c <= 5);
	show_full_pv = false;
	pre_calculate();
	clear_history();
	
	/*generate root moves here*/
	if(in_egbb) {
		if(probe_bitbases(scorer));
		else in_egbb = false;
	}

	pstack->count = 0;
	gen_all();
	legal_moves = 0;
	for(i = 0;i < pstack->count; i++) {
		pstack->current_move = pstack->move_st[i];
		PUSH_MOVE(pstack->current_move);
		if(attacks(player,plist[COMBINE(opponent,king)]->sq)) {
			POP_MOVE();
			continue;
		}
		if(in_egbb) {
			if(probe_bitbases(score)) {
				score = -score;
				if(score - scorer >= -1000);
				else {
					POP_MOVE();
					continue;
				}
			} else {
				in_egbb = false;
				score = 0;
			}
		} else {
			score = 0;
		}
		POP_MOVE();

		pstack->move_st[legal_moves] = pstack->current_move;
		pstack->score_st[legal_moves] = score;
		root_score_st[legal_moves] = 0;
		legal_moves++;
	}

	pstack->count = legal_moves;

	/*loss or draw*/
	if(in_egbb) {
		for(i = 0;i < pstack->count; i++) {
			pstack->sort(i,pstack->count);
		}
		if(pstack->count && pstack->score_st[0] <= 0) {
			pstack->count = 1;
		}
	}

	/*no move*/
	if(pstack->count == 0) {
		print("no moves\n");
		return 0;
	}
	/*only one move*/
	if(pstack->count == 1) {
		pstack->pv_length = 1;
		pstack->pv[0] = pstack->move_st[0];
		print_pv(0);
		return pstack->move_st[0];
	}

#ifdef BOOK_PROBE
	/*find book move*/
	if(book_loaded 
		&& hply <= last_book_move + 6 
		) {
			MOVE move = get_book_move();
			if(move) {
				last_book_move = hply;
				pstack->pv_length = 1;
				pstack->pv[0] = move;
				print_pv(0);
				return move;
			}
	}
#endif

	/*set search time*/
	if(!chess_clock.infinite_mode)
		chess_clock.set_stime(hply);

	/*
	* Send initial position to hosts. This is achived by
	* sending FEN string and previous non-retractable moves.
	*/

#ifdef CLUSTER
	int len,move;
	INIT_MESSAGE init;

	/*undo to last fifty move*/       
	len = fifty + 1;
	for(i = 0;i < len && hply > 0;i++) {
		if(hstack[hply - 1].move) undo_move();
		else undo_null();
	}
	get_fen((char*)init.fen);

	/*redo moves*/
	len = i;
	init.pv_length = 0;
	for(i = 0;i < len;i++) {
		move = hstack[hply].move;
		init.pv[init.pv_length++] = move;
		if(move) do_move(move);
		else do_null();
	}
	/*send initial position*/
	for(i = 0;i < PROCESSOR::n_hosts;i++) {
		if(i != PROCESSOR::host_id)
			PROCESSOR::ISend(i,PROCESSOR::INIT,&init,INIT_MESSAGE_SIZE(init));
	}
#endif

	/*wakeup threads*/
#ifdef PARALLEL
	for(i = 1;i < PROCESSOR::n_processors;i++) {
		processors[i]->state = WAIT;
	}
#endif

	/*score non-egbb moves*/
	probe_depth = 0;
	if(!in_egbb) {
		for(i = 0;i < pstack->count; i++) {
			pstack->current_move = pstack->move_st[i];
			PUSH_MOVE(pstack->current_move);
			pstack->alpha = -MATE_SCORE;
			pstack->beta = MATE_SCORE;
			pstack->depth = 0;
			pstack->qcheck_depth = CHECK_DEPTH;	
			qsearch();
			POP_MOVE();
			pstack->score_st[i] = -(pstack + 1)->best_score;
		}
		for(i = 0;i < pstack->count; i++) {
			pstack->sort(i,pstack->count);
		}
	}

	/*easy move*/
	if(pstack->score_st[0] > pstack->score_st[1] + 175
		&& chess_clock.is_timed()
		) {
			easy = true;
			easy_move = pstack->move_st[0];
			easy_score = pstack->score_st[0];
			chess_clock.search_time /= 4;
	}

	stack[0].pv[0] = pstack->move_st[0];

	/*iterative deepening*/
	int alpha,beta,WINDOW = 25;
	alpha = pstack->score_st[0] - 4 * WINDOW;
	beta = pstack->score_st[0] + 4 * WINDOW;
	root_failed_low = 0;
	do {

		/*search with the current depth*/
		search_depth++;

		/*use the whole search depth once inegbb*/
		if(in_egbb) probe_depth = search_depth;
		else probe_depth = (2 * search_depth) / 3;

		/*Set bounds and search.*/
		pstack->depth = search_depth * UNITDEPTH;
		pstack->alpha = alpha;
		pstack->beta = beta;
		root_search();
		if(abort_search)
			break;
		/*score*/
		score = pstack->best_score;

		/*fail low at root*/
		if(root_failed_low && score > alpha) {
			root_failed_low--;
			if(root_failed_low && prev_root_score - root_score < 50) 
				root_failed_low--;
		}

		/*install fake pv into TT table so that it is searched
		first incase it was overwritten*/	
		if(pstack->pv_length) {
			for(i = 0;i < stack[0].pv_length;i++) {
				record_hash(player,hash_key,0,0,CRAP,0,stack[0].pv[i],0);
				PUSH_MOVE(stack[0].pv[i]);
			}
			for(i = 0;i < stack[0].pv_length;i++)
				POP_MOVE();
		}

		/*sort moves*/
		int j;
		MOVE tempm;
		UBMP64 temps,bests = 0;

		for(i = 0;i < pstack->count; i++) {
			if(pstack->pv[0] == pstack->move_st[i]) {
				bests = root_score_st[i];
				root_score_st[i] = MAX_UBMP64;
			}
		}
		for(i = 0;i < pstack->count; i++) {
			for(j = i + 1;j < pstack->count;j++) {
				if(root_score_st[i] < root_score_st[j]) {
					tempm = pstack->move_st[i];
					temps = root_score_st[i];
					pstack->move_st[i] = pstack->move_st[j];
					root_score_st[i] = root_score_st[j];
					pstack->move_st[j] = tempm;
					root_score_st[j] = temps;
				}
			}
		}
		for(i = 0;i < pstack->count; i++) {
			if(pstack->pv[0] == pstack->move_st[i]) {
				root_score_st[i] = bests;
			}
		}

		/*Is there enough time to search the first move?*/
		if(!root_failed_low && chess_clock.is_timed()) {
			time_used = get_time() - start_time;
			if(time_used >= 0.75 * chess_clock.search_time) {
				abort_search = 1;
				break;
			}
		}

		/*check if "easy" move is really easy*/
		if(easy && (easy_move != pstack->pv[0] || score <= easy_score - 60)) {
			easy = false;
			chess_clock.search_time *= 4;
		}

		/*aspiration search*/
		if(in_egbb || ABS(score) >= 1000 || search_depth <= 3) {
			alpha = -MATE_SCORE;
			beta = MATE_SCORE;
		} else if(score <= alpha) {
			WINDOW = MIN(200, 4 * WINDOW);
			alpha = MAX(-MATE_SCORE,score - WINDOW);
			search_depth--;
		} else if (score >= beta){
			WINDOW = MIN(200, 4 * WINDOW);
			beta = MIN(MATE_SCORE,score + WINDOW);
			search_depth--;
		} else {
			WINDOW = 10;
			alpha = score - WINDOW;
			beta = score + WINDOW;
		}

		/*check time*/
		check_quit();

		/*store info*/
		prev_root_score = root_score;
		/*end*/

	} while(search_depth < chess_clock.max_sd);

	/*search has ended. display some info*/
	time_used = get_time() - start_time;
	if(!time_used) time_used = 1;
	if(pv_print_style == 1) {
		print(" "FMT64W" %8.2f %10d %8d %8d\n",nodes,float(time_used) / 1000,
			int(BMP64(nodes) / (time_used / 1000.0f)),splits,bad_splits);
	} else {
		print("nodes = "FMT64" <%d qnodes> time = %dms nps = %d\n",nodes,
			int(BMP64(qnodes) / (BMP64(nodes) / 100.0f)),
			time_used,int(BMP64(nodes) / (time_used / 1000.0f)));
		print("splits = %d badsplits = %d egbb_probes = %d\n",
			splits,bad_splits,egbb_probes);
	}
#ifdef CLUSTER
	/*park hosts*/
	for(i = 0;i < PROCESSOR::n_hosts;i++) {
		if(i != PROCESSOR::host_id)
			PROCESSOR::ISend(i,PROCESSOR::RELAX);
	}
#endif
#ifdef PARALLEL
	/*park threads*/
	for(i = 1;i < PROCESSOR::n_processors;i++) {
		processors[i]->state = PARK;
	}
#endif

	/*was this first search?*/
	if(first_search) {
		first_search = false;
	}

	return stack[0].pv[0];
}

/*
* Search parameters
*/
bool check_search_params(char** commands,char* command,int& command_num) {
	if(!strcmp(command, "use_iid")) {
		use_iid = atoi(commands[command_num++]);
	} else if(!strcmp(command, "futility_margin")) {
		futility_margin = atoi(commands[command_num++]);
	} else if(!strcmp(command, "use_singular")) {
		use_singular = atoi(commands[command_num++]);
	} else if(!strcmp(command, "singular_margin")) {
		singular_margin = atoi(commands[command_num++]);
	} else if(!strcmp(command, "smp_depth")) {
		SMP_CODE(PROCESSOR::SMP_SPLIT_DEPTH = atoi(commands[command_num]));
		command_num++;
	} else if(!strcmp(command, "cluster_depth")) {
		CLUSTER_CODE(PROCESSOR::CLUSTER_SPLIT_DEPTH = atoi(commands[command_num]));
		command_num++;
	} else if(!strcmp(command, "message_poll_nodes")) {
		CLUSTER_CODE(PROCESSOR::MESSAGE_POLL_NODES = atoi(commands[command_num]));
		command_num++;
	} else {
		return false;
	}
	return true;
}
void print_search_params() {
	SMP_CODE(print("feature option=\"smp_depth -spin %d 1 10\"\n",PROCESSOR::SMP_SPLIT_DEPTH));
	CLUSTER_CODE(print("feature option=\"cluster_depth -spin %d 1 16\"\n",PROCESSOR::CLUSTER_SPLIT_DEPTH));
	CLUSTER_CODE(print("feature option=\"message_poll_nodes -spin %d 10 20000\"\n",PROCESSOR::MESSAGE_POLL_NODES));
	print("feature option=\"use_iid -check %d\"\n",use_iid);
	print("feature option=\"use_singular -check %d\"\n",use_singular);
	print("feature option=\"singular_margin -spin %d 0 1000\"\n",singular_margin);
	print("feature option=\"futility_margin -spin %d 0 1000\"\n",futility_margin);
}
