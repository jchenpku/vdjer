/* Copyright 2013 University of North Carolina at Chapel Hill.  All rights reserved. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <pthread.h>
#include <unistd.h>
#include <iostream>
#include <stack>
#include <list>
#include <utility>
#include <vector>
#include <sparsehash/sparse_hash_map>
#include <sparsehash/sparse_hash_set>
#include <sparsehash/dense_hash_map>
#include <sparsehash/dense_hash_set>
#include <stdexcept>
#include "status.h"
#include "seq_score.h"
#include "hash_utils.h"
#include "vj_filter.h"
#include "quick_map3.h"

using namespace std;
using google::sparse_hash_map;
using google::sparse_hash_set;
using google::dense_hash_map;
using google::dense_hash_set;

// bam_read.c
extern void extract(char* bam_file, char* vdj_fasta, char* v_region, char* c_region,
		char*& primary_buf, char*& secondary_buf);

// coverage.c
extern char coverage_is_valid(int read_length, int contig_len, int eval_start, int eval_stop, int read_span,
		               int insert_low, int insert_high, int floor, vector<mapped_pair>& mapped_reads, vector<pair<int,int> >& start_positions, char is_debug);

// quick_map3.c
extern void quick_map_process_contig(char* contig_id, char* contig, vector<mapped_pair>& mapped_reads,
		vector<pair<int,int> >& start_positions);

// quick_map3.c
extern void quick_map_process_contig_file(char* contig_file);

#define MIN_CONTIG_SIZE 500
#define MAX_CONTIG_SIZE 600
#define MAX_READ_LENGTH 1001
#define MAX_FRAGMENT_SIZE 100
//#define MIN_BASE_QUALITY 20
#define INCREASE_MIN_NODE_FREQ_THRESHOLD 2500

#define MAX_TOTAL_CONTIG_LEN 10000000

#define OK 0
#define TOO_MANY_PATHS_FROM_ROOT -1
#define TOO_MANY_CONTIGS -2
#define STOPPED_ON_REPEAT -3
#define TOO_MANY_NODES -4

#define MAX_FREQUENCY 32766
#define MAX_QUAL_SUM 255

//#define MIN_HOMOLOGY_SCORE 13

// TODO: This is used to bound qual sum arrays.  Use a memory pool instead for this.
#define MAX_KMER_LEN 50

// This makes sense for small assembly windows, but should be parameterized for larger assemblies
#define MAX_NODES 900000000

// Kmers containing bases below this threshold are excluded from assembly.
#define MIN_BASE_QUALITY 20

// Minimum edge frequency as percent
// #define MIN_EDGE_FREQUENCY .02

#define MIN_EDGE_FREQUENCY -1

// TODO: This should vary be kmer len
//#define MIN_ROOT_HOMOLOGY_SCORE 25

// Must be greater than the number of  source(root) nodes - TODO: re-use threads and allocate dynamically.
#define MAX_THREADS 1000000

#define MAX_NODE_VISITS 5

#define VREGION_BUF_MAX 10000000

int MIN_ROOT_HOMOLOGY_SCORE;

int MAX_RUNNING_THREADS;

pthread_mutex_t running_thread_mutex;
pthread_mutex_t contig_writer_mutex;

int running_threads = 0;

// Tracks vjf windows
dense_hash_set<const char*, contig_hash, contig_eqstr> vjf_windows;

dense_hash_set<const char*, vjf_hash, vjf_eqstr> vjf_window_candidates;

// #define MAX_TRACEBACK_STACK_SIZE 1024
#define MAX_TRACEBACK_STACK_SIZE 256


//TODO: Better variable localization
//__thread int read_length;
//__thread int min_contig_length;
//__thread int kmer_size;
//__thread int min_node_freq;
//__thread int min_base_quality;

int read_length;
int kmer_size;
int min_node_freq;
int min_base_quality;

//#define MIN_CONTIG_SCORE -6
float MIN_CONTIG_SCORE;

int CONTIG_SIZE;
int INSERT_LEN;
int FILTER_READ_FLOOR;
//int MIN_ROOT_SIMILARITY;
char* ROOT_SIMILARITY_FILE;
int VREGION_KMER_SIZE;

struct struct_pool {
//	struct node_pool* node_pool;
//	struct read_pool* read_pool;
};

//#define NODES_PER_BLOCK 5000000
//#define MAX_NODE_BLOCKS 50000
//#define READS_PER_BLOCK 1000000
//#define MAX_READ_BLOCKS 100000

//struct node_pool {
//	struct node** nodes;
//	int block_idx;
//	int node_idx;
//};
//
//struct read_pool {
//	char** reads;
//	int block_idx;
//	int read_idx;
//};

struct node {

	//TODO: Collapse from 8 to 2 bits.  Only store as key.
	char* kmer;
	//TODO: Convert to stl?
	struct linked_node* toNodes;
	struct linked_node* fromNodes;
	unsigned short frequency;
};

struct pre_node {
	char* contributingRead;
	unsigned char qual_sums[MAX_KMER_LEN];
	unsigned short frequency;
	char hasMultipleUniqueReads;
	char contributing_strand;
};

struct linked_node {
	struct node* node;
	struct linked_node* next;
};

int compare_read(const char* s1, const char* s2) {
	return (s1 == s2) || (s1 && s2 && strncmp(s1, s2, read_length) == 0);
}

int compare_kmer(const char* s1, const char* s2) {
	return (s1 == s2) || (s1 && s2 && strncmp(s1, s2, kmer_size) == 0);
}

unsigned char phred33(char ch) {
	return ch - '!';
}

void print_kmer(char* kmer) {
    for (int i=0; i<kmer_size; i++) {
    	fprintf(stderr, "%c", kmer[i]);
    }
}

void print_kmer(struct node* node) {
	fprintf(stderr, "%x\t", node);
    for (int i=0; i<kmer_size; i++) {
    	fprintf(stderr, "%c", node->kmer[i]);
    }
}

void print_node(struct node* node) {
        fprintf(stderr, "kmer: ");
        print_kmer(node);
        fprintf(stderr, "\tfrom: ");

        struct linked_node* from = node->fromNodes;
        while (from != NULL) {
        	print_kmer(from->node);
        	fprintf(stderr, ",");
        	from = from->next;
        }

        fprintf(stderr, "\tto: ");
        struct linked_node* to = node->toNodes;
        while (to != NULL) {
        	print_kmer(to->node);
        	fprintf(stderr, ",");
        	to = to->next;
        }
}

struct node* new_node(char* seq, char* contributingRead, struct_pool* pool, int strand, char* quals) {

	node* my_node = (node*) calloc(1, sizeof(struct node));
	my_node->kmer = seq;
//	my_node->contributingRead = contributingRead;
//	my_node->frequency = 1;
//	my_node->hasMultipleUniqueReads = 0;
//	my_node->contributing_strand = (char) strand;
//	for (int i=0; i<kmer_size; i++) {
//		my_node->qual_sums[i] = phred33(quals[i]);
//	}
	return my_node;
}

char* get_kmer(int idx, char* sequence) {
	return &sequence[idx];
}

int is_node_in_list(struct node* node, struct linked_node* list) {
	struct linked_node* ptr = list;

	while (ptr != NULL) {
		if (compare_kmer(ptr->node->kmer, node->kmer)) {
			return 1;
		}
		ptr = ptr->next;
	}

	return 0;
}

void link_nodes(struct node* from_node, struct node* to_node) {
	if (!is_node_in_list(to_node, from_node->toNodes)) {
		struct linked_node* to_link = (linked_node*) malloc(sizeof(linked_node));
		to_link->node = to_node;
		to_link->next = from_node->toNodes;
		from_node->toNodes = to_link;
	}

	if (!is_node_in_list(from_node, to_node->fromNodes)) {
		struct linked_node* from_link = (linked_node*) malloc(sizeof(linked_node));
		from_link->node = from_node;
		from_link->next = to_node->fromNodes;
		to_node->fromNodes = from_link;
	}
}


int include_kmer(char* sequence, char*qual, int idx) {

	int include = 1;

	for (int i=idx; i<idx+kmer_size; i++) {
		// Discard kmers with ambiguous bases
		if (sequence[i] == 'N') {
			include = 0;
			break;
		}

		// Discard kmers with low base qualities
		if (phred33(qual[i]) < MIN_BASE_QUALITY) {
			include = 0;
			break;
		}
	}

	return include;
}

void increment_node_freq(struct node* node) {
	if (node->frequency < MAX_FREQUENCY-1) {
		node->frequency++;
	}
}

void add_to_graph(char* sequence, dense_hash_map<const char*, struct node*, my_hash, eqstr>* nodes, struct_pool* pool, char* qual, int strand, char has_roots,
		sparse_hash_map<const char*, pre_node, my_hash, eqstr>& pre_nodes) {

	struct node* prev = 0;

	for (int i=0; i<=read_length-kmer_size; i++) {

		if (pre_nodes.find(sequence+i) != pre_nodes.end()) {
			char* kmer = get_kmer(i, sequence);
			char* kmer_qual = get_kmer(i, qual);

			struct node* curr = NULL;

			if (nodes->find(kmer) == nodes->end()) {
				curr = new_node(kmer, sequence, pool, strand, kmer_qual);

				if (curr == NULL) {
					fprintf(stderr, "Null node for kmer: %s\n", kmer);
					exit(-1);
				}

				(*nodes)[kmer] = curr;
			} else {
				curr = (*nodes)[kmer];
				increment_node_freq(curr);
			}

			if (prev != NULL) {
				link_nodes(prev, curr);
			}

			prev = curr;
		} else {
			prev = NULL;
		}
	}
}

void add_to_table(char* sequence, sparse_hash_map<const char*, pre_node, my_hash, eqstr> & pre_table, char* qual, int strand) {

	for (int i=0; i<=read_length-kmer_size; i++) {

		if (include_kmer(sequence, qual, i)) {
			char* kmer = get_kmer(i, sequence);
			char* kmer_qual = get_kmer(i, qual);

			pre_node node;

			if (pre_table.find(kmer) == pre_table.end()) {
				node.contributingRead = sequence;
				node.frequency = 1;
				node.hasMultipleUniqueReads = 0;
				node.contributing_strand = (char) strand;
				for (int i=0; i<kmer_size; i++) {
					node.qual_sums[i] = phred33(qual[i]);
				}

				pre_table[kmer] = node;
			} else {
				node = pre_table[kmer];

				if (node.frequency < MAX_FREQUENCY-1) {
					node.frequency++;
				}

				if (!(node.hasMultipleUniqueReads) &&
					(!compare_read(node.contributingRead, sequence) || node.contributing_strand != (char) strand)) {
					node.hasMultipleUniqueReads = 1;
				}

				for (int i=0; i<kmer_size; i++) {
					unsigned char phred33_qual = phred33(kmer_qual[i]);
					if ((node.qual_sums[i] + phred33_qual) < MAX_QUAL_SUM-41) {
						node.qual_sums[i] += phred33_qual;
					} else {
						node.qual_sums[i] = MAX_QUAL_SUM;
					}
				}

				pre_table[kmer] = node;
			}
		}
	}
}

void build_pre_graph(const char* input, sparse_hash_map<const char*, pre_node, my_hash, eqstr>& pre_nodes) {
	size_t input_len = strlen(input);
	int record_len = read_length*2 + 1;

	fprintf(stderr, "input_len: %ld, record_len: %d\n", input_len, record_len);
	size_t num_records = input_len / record_len;
	size_t record = 0;
	const char* ptr = input;
	int num_reads = 0;

	while ((record < num_records) && (pre_nodes.size() < MAX_NODES)) {
		ptr = &(input[record*record_len]);
		int strand = 0;

		if (ptr[0] == '0') {
			strand = 0;
		} else if (ptr[0] == '1') {
			strand = 1;
		} else {
			fprintf(stderr, "Initial char in input invalid: %c\n", ptr[0]);
			fprintf(stderr, "ERROR!  INVALID INPUT:\n===========================%s\n===========================\n", input);
			exit(-1);
		}

		char* read_ptr = (char*) &(ptr[1]);
		char* qual_ptr = (char*) &(ptr[read_length+1]);

		// Add to hash table
		add_to_table(read_ptr, pre_nodes, qual_ptr, strand);
		record++;

		if ((record % 1000000) == 0) {
			fprintf(stderr, "pre_record_count: %d\n", record);
			fflush(stdout);
		}
	}

	fprintf(stderr, "Pre Num reads: %d\n", record);
	fprintf(stderr, "Pre Num nodes: %d\n", pre_nodes.size());
	fflush(stderr);
}


void build_graph2(const char* input, dense_hash_map<const char*, struct node*, my_hash, eqstr>* nodes, struct_pool* pool, char has_roots,
		sparse_hash_map<const char*, pre_node, my_hash, eqstr>& pre_nodes) {
	size_t input_len = strlen(input);
	int record_len = read_length*2 + 1;

	fprintf(stderr, "input_len: %ld, record_len: %d\n", input_len, record_len);
	size_t num_records = input_len / record_len;
	size_t record = 0;
	const char* ptr = input;
	int num_reads = 0;

	while ((record < num_records) && (nodes->size() < MAX_NODES)) {
		ptr = &(input[record*record_len]);
		int strand = 0;

		if (ptr[0] == '0') {
			strand = 0;
		} else if (ptr[0] == '1') {
			strand = 1;
		} else {
			fprintf(stderr, "Initial char in input invalid: %c\n", ptr[0]);
			fprintf(stderr, "ERROR!  INVALID INPUT:\n===========================%s\n===========================\n", input);
			exit(-1);
		}

		char* read_ptr = (char*) &(ptr[1]);

		char* qual_ptr = (char*) &(ptr[read_length+1]);
		add_to_graph(read_ptr, nodes, pool, qual_ptr, strand, has_roots, pre_nodes);
		record++;

		if ((record % 1000000) == 0) {
			fprintf(stderr, "record_count: %d\n", record);
			fflush(stdout);
		}
	}

	fprintf(stderr, "Num reads: %d\n", record);
	fprintf(stderr, "Num nodes: %d\n", nodes->size());
	fflush(stderr);
}

int is_base_quality_good(unsigned char* qual_sums) {
	int is_good = 1;

	for (int i=0; i<kmer_size; i++) {
		if (qual_sums[i] < min_base_quality) {
			is_good = 0;
			break;
		}
	}

	return is_good;
}

void prune_pre_graph(sparse_hash_map<const char*, pre_node, my_hash, eqstr>& pre_nodes) {

	pre_nodes.set_deleted_key(NULL);
	for (sparse_hash_map<const char*, pre_node, my_hash, eqstr>::const_iterator it = pre_nodes.begin();
			it != pre_nodes.end(); ++it) {

		const char* key = it->first;
		pre_node node = it->second;

//		fprintf(stderr, "prenode: %d\t%d\t%d\t%d\n", node.frequency, node.hasMultipleUniqueReads, node.qual_sums[0], node.qual_sums[34]);

		if ((node.frequency < min_node_freq) ||
			!(node.hasMultipleUniqueReads) ||
			!is_base_quality_good(node.qual_sums)) {

			pre_nodes.erase(key);
		}
	}

	pre_nodes.resize(0);
}

int num_root_candidates = 0;

char has_vregion_homology(char* kmer, dense_hash_set<const char*, vregion_hash, vregion_eqstr>& contig_index) {

	char is_homologous = 0;

	if (VREGION_KMER_SIZE > 1) {
		int stop =  kmer_size - VREGION_KMER_SIZE;

		// Identify hits in hash index
		for (int i=0; i<stop; i++) {
			if (contig_index.find(kmer+i) != contig_index.end()) {
				is_homologous = 1;
				break;
			}
		}
	} else {
		is_homologous = 1;
	}

	return is_homologous;
}

void add_to_index(char* contig, dense_hash_set<const char*, vregion_hash, vregion_eqstr>& contig_index) {
	int stop = strlen(contig)-VREGION_KMER_SIZE;
	for (int i=0; i<stop; i++) {
		if (contig_index.find(contig+i) == contig_index.end()) {
			contig_index.insert(contig+i);
		}
	}
}

void load_root_similarity_index(dense_hash_set<const char*, vregion_hash, vregion_eqstr>& contig_index) {
//	contig_index.set_deleted_key(NULL);
	char* contig = (char*) calloc(VREGION_BUF_MAX, sizeof(char));

	FILE* fp = fopen(ROOT_SIMILARITY_FILE, "r");

	int seq_found = 0;

	while (fgets(contig, 1000000, fp) != NULL) {
		if (contig[0] != '>') {
			// Get rid of newline
			contig[strlen(contig)-1] = '\0';
			add_to_index(contig, contig_index);
			seq_found++;
		}
	}

	assert(seq_found == 1);
}



int is_root(struct node* node, int& num_root_candidates) {
	int is_root = 0;

	if (node != NULL) {
		if (node->fromNodes == NULL) {
			num_root_candidates += 1;
			is_root = 1;

			fprintf(stderr, "ROOT_INIT:\t%d\t", node->frequency);
			print_kmer(node->kmer);
			fprintf(stderr, "\n");

		} else {
			// Identify nodes that point to themselves with no other incoming edges.
			// This will be cleaned up during contig building.
			struct linked_node* from = node->fromNodes;
			if (from->next == NULL && (strncmp(node->kmer, from->node->kmer, kmer_size) == 0)) {
				fprintf(stderr, "SELF_ROOT\n");
			}
		}
	}

	return is_root;

}


struct linked_node* identify_root_nodes(dense_hash_map<const char*, struct node*, my_hash, eqstr>* nodes) {

	struct linked_node* root_nodes = NULL;
	int count = 0;
	int num_root_candidates = 0;

	for (dense_hash_map<const char*, struct node*, my_hash, eqstr>::const_iterator it = nodes->begin();
	         it != nodes->end(); ++it) {
		struct node* node = it->second;

		if (is_root(node, num_root_candidates)) {
			struct linked_node* next = root_nodes;
			root_nodes = (linked_node*) malloc(sizeof(linked_node));
			root_nodes->node = node;
			root_nodes->next = next;

			count++;
		}
	}

	fprintf(stderr, "num root nodes: %d\n", count);

	fprintf(stderr, "NUM_ROOT_CANDIDATES: %d\n", num_root_candidates);
	fflush(stderr);

	return root_nodes;
}

struct contig {
	char* seq;
	vector<char*>* fragments;
	struct node* curr_node;
	dense_hash_map<const char*, char, my_hash, eqstr>* visited_nodes;
	double score;
	int size;  // really curr_index now
	int real_size;
	char is_repeat;
};

struct contig* new_contig() {
	struct contig* curr_contig;
	curr_contig = (contig*) calloc(1, sizeof(contig));
	curr_contig->seq = (char*) calloc(MAX_FRAGMENT_SIZE, sizeof(char));
	curr_contig->size = 0;
	curr_contig->is_repeat = 0;
	curr_contig->visited_nodes = new dense_hash_map<const char*, char, my_hash, eqstr>();
	curr_contig->visited_nodes->set_empty_key(NULL);
//	curr_contig->visited_nodes->resize(MAX_CONTIG_SIZE);
	curr_contig->score = 0;
	curr_contig->fragments = new vector<char*>();

	return curr_contig;
}

struct contig* copy_contig(struct contig* orig, vector<char*>& all_contig_fragments) {

	// Stash any orig sequence in fragment vector so the pointer can be shared across contigs
	if (strlen(orig->seq) > 0) {
		orig->fragments->push_back(orig->seq);

		// track fragments for cleanup later
		all_contig_fragments.push_back(orig->seq);

		orig->seq = (char*) calloc(MAX_FRAGMENT_SIZE, sizeof(char));
		orig->size = 0;  // Reset index (not really size)
	}

	struct contig* copy = (contig*) calloc(sizeof(contig), sizeof(char));
	copy->seq = (char*) calloc(MAX_FRAGMENT_SIZE, sizeof(char));

	// Copy original fragments to new contig
	copy->fragments = new vector<char*>(*(orig->fragments));

	copy->size = orig->size;
	copy->real_size = orig->real_size;
	copy->is_repeat = orig->is_repeat;
	//copy->visited_nodes = new dense_hash_map<const char*, char, my_hash, eqstr>(*orig->visited_nodes);
	copy->visited_nodes = new dense_hash_map<const char*, char, my_hash, eqstr>();
	copy->score = orig->score;

	return copy;

}

void free_contig(struct contig* contig) {
	delete contig->visited_nodes;
	delete contig->fragments;
	free(contig->seq);
	free(contig);
}

char contains_visited_node(struct contig* contig, struct node* node) {
//	dense_hash_map<const char*, char, my_hash, eqstr>::const_iterator it = contig->visited_nodes->find(node->kmer);
//	return it != contig->visited_nodes->end();

	return 0;
}

char is_node_visited(struct contig* contig, struct node* node) {
	char is_visited = 0;
	if (contains_visited_node(contig, node)) {
		dense_hash_map<const char*, char, my_hash, eqstr>* vnodes = contig->visited_nodes;
		if ((*vnodes)[node->kmer] > MAX_NODE_VISITS) {
			is_visited = 1;
		}
	}

	return is_visited;
}

void visit_curr_node(struct contig* contig) {
	char num_visits = 1;
	dense_hash_map<const char*, char, my_hash, eqstr>* vnodes = contig->visited_nodes;

	if (contains_visited_node(contig, contig->curr_node)) {
		num_visits = (*vnodes)[contig->curr_node->kmer] + 1;
	}

	(*vnodes)[contig->curr_node->kmer] = num_visits;
}

int get_contig_len(struct contig* contig) {
	int length = strlen(contig->seq);

	for (vector<char*>::iterator it = contig->fragments->begin(); it != contig->fragments->end(); ++it) {
		length += strlen(*it);
	}

	return length;
}

int output_contigs = 0;

char contains_seq(dense_hash_set<const char*, vjf_hash, vjf_eqstr>& seq_set, char* seq) {
	dense_hash_set<const char*, vjf_hash, vjf_eqstr>::const_iterator it = seq_set.find(seq);
	return it != seq_set.end();
}

char contains_seq(dense_hash_set<const char*, contig_hash, contig_eqstr>& seq_set, char* seq) {
	dense_hash_set<const char*, contig_hash, contig_eqstr>::const_iterator it = seq_set.find(seq);
	return it != seq_set.end();
}


int contig_num = 1;
int total_contigs = 0;

void output_contig(struct contig* contig, int& contig_count, const char* prefix, char* contigs) {

	if (contig->real_size >= MIN_CONTIG_SIZE) {
		char buf[MAX_CONTIG_SIZE+1];
		total_contigs += 1;
		if ((total_contigs % 100000) == 0) {
			fprintf(stderr, "contig_candidates: %d\n", total_contigs);
		}
		contig_count++;

		buf[0] = '\0';

		for (vector<char*>::iterator it = contig->fragments->begin(); it != contig->fragments->end(); ++it) {
			strcat(buf, *it);
		}

		if (strlen(contig->seq) > 0) {
			strcat(buf, contig->seq);
		}


		// Search for V / J anchors and add to hash set.
		dense_hash_set<const char*, vjf_hash, vjf_eqstr> vjf_windows_temp;
		vjf_windows_temp.set_empty_key(NULL);
		vjf_search(buf, vjf_windows_temp);

//		fprintf(stderr, "CONTIG_CANDIDATE: %s\t%d\n", buf, vjf_windows_temp.size());

		for (dense_hash_set<const char*, vjf_hash, vjf_eqstr>::iterator it=vjf_windows_temp.begin(); it!=vjf_windows_temp.end(); it++) {
			char is_to_be_processed = 0;
			char contig_id[256];

			CONTIG_SIZE = 341;
			int eval_start = 50;
			int eval_stop  = 390;
			int read_span  = 35;
			int insert_low = INSERT_LEN;
			int insert_high = INSERT_LEN;
			int floor = FILTER_READ_FLOOR;

			// TODO: Use RW lock here?
			pthread_mutex_lock(&contig_writer_mutex);
			if (!contains_seq(vjf_window_candidates, (char*) *it) && !contains_seq(vjf_windows, ((char*) *it) + (eval_start-1))) {
				is_to_be_processed = 1;
				// Don't process same window twice
				vjf_window_candidates.insert(*it);
				sprintf(contig_id, "vjf_%d", contig_num++);
				if ((contig_num % 1000) == 0) {
					fprintf(stderr, "Processing contig num: %d\n", contig_num);
				}

//				fprintf(stderr, "PROCESS_CONTIG: %s\t%s\n", contig_id, *it);
			}
			pthread_mutex_unlock(&contig_writer_mutex);

			if (is_to_be_processed) {
				vector<mapped_pair> mapped_reads;
				vector<pair<int,int> > start_positions;

				quick_map_process_contig(contig_id, (char*) *it, mapped_reads, start_positions);

				char is_debug = 0;

				char is_valid = coverage_is_valid(read_length, strlen(*it),
						eval_start, eval_stop, read_span, insert_low, insert_high, floor, mapped_reads, start_positions, is_debug);

				if (is_valid) {
//					fprintf(stderr, "VALID_CONTIG: %s\t%d\n", *it, mapped_reads.size());

					// Truncate assembled contig at eval stop
					((char*)(*it))[eval_start+CONTIG_SIZE-1] = '\0';

					// Add contig to set
					pthread_mutex_lock(&contig_writer_mutex);
					vjf_windows.insert(((char*) *it) + (eval_start-1));
					pthread_mutex_unlock(&contig_writer_mutex);
				} else {
//					fprintf(stderr, "INVALID_CONTIG: %s\t%d\n", *it, mapped_reads.size());
				}
			} else {
				// TODO: Re-use buffers here...
				free((char*) *it);
			}
		}
	}
}

void output_windows() {

	char* contig_file = "vdj_contigs.fa";
	FILE* fp = fopen(contig_file, "w");
	int contig_num = 1;
	for (dense_hash_set<const char*, contig_hash, contig_eqstr>::iterator it=vjf_windows.begin(); it!=vjf_windows.end(); it++) {
		fprintf(fp, ">vjf_%d\n%s\n", contig_num++, *it);
	}
	fclose(fp);

	fprintf(stderr, "Outputting SAM\n");
	quick_map_process_contig_file(contig_file);
	fprintf(stderr, "SAM output done.\n");
}

void append_to_contig(struct contig* contig, vector<char*>& all_contig_fragments, char entire_kmer) {
	int add_len = 1;
	if (entire_kmer) {
		add_len = kmer_size;
	}

	if (contig->size+add_len < MAX_FRAGMENT_SIZE) {
		contig->fragments->push_back(contig->seq);

		// track fragments for cleanup later
		all_contig_fragments.push_back(contig->seq);

		contig->seq = (char*) calloc(MAX_FRAGMENT_SIZE, sizeof(char));
		contig->size = 0;  // Reset index (not really size)
	}

	if (!entire_kmer) {
		contig->seq[contig->size++] = contig->curr_node->kmer[0];
		contig->real_size += 1;
	} else {
		strncpy(&(contig->seq[contig->size]), contig->curr_node->kmer, kmer_size);
		contig->size += kmer_size;
		contig->real_size += kmer_size;
	}
}

int build_contigs(
		struct node* root,
		int& contig_count,
		const char* prefix,
		int max_paths_from_root,
		int max_contigs,
		char stop_on_repeat,
		char shadow_mode,
		char* contig_str) {

	int status = OK;
	stack<contig*> contigs;
	stack<contig*> popped_contigs;
	struct contig* root_contig = new_contig();
	root_contig->curr_node = root;
	contigs.push(root_contig);

	// Track all contig fragments
	// Initialize to reasonably large number to avoid reallocations
	int INIT_FRAGMENTS_PER_THREAD = 10000000;
	vector<char*> all_contig_fragments;
	all_contig_fragments.reserve(INIT_FRAGMENTS_PER_THREAD);

	int paths_from_root = 1;

	while ((contigs.size() > 0) && (status == OK)) {
		// Get contig from stack
		struct contig* contig = contigs.top();

		if (is_node_visited(contig, contig->curr_node)) {
			fprintf(stderr, "Repeat node: ");
			print_kmer(contig->curr_node);
			fprintf(stderr, "\n");
			// We've encountered a repeat
			contig->is_repeat = 1;
			if ((!shadow_mode) && (!stop_on_repeat)) {
				output_contig(contig, contig_count, prefix, contig_str);
				free_contig(contig);
			} else {
				popped_contigs.push(contig);
			}
			contigs.pop();
			if (stop_on_repeat) {
				status = STOPPED_ON_REPEAT;
			}
		}
		else if (contig->curr_node->toNodes == NULL || contig->score < MIN_CONTIG_SCORE || contig->real_size >= (MAX_CONTIG_SIZE-kmer_size-1)) {
			// We've reached the end of the contig.
			// Append entire current node.
			append_to_contig(contig, all_contig_fragments, 1);

			// Now, write the contig
			if (!shadow_mode) {
				// only output at sink node
				output_contig(contig, contig_count, prefix, contig_str);
				free_contig(contig);
			} else {
				popped_contigs.push(contig);
			}
			contigs.pop();
		}
		else {
			// Append first base from current node
			append_to_contig(contig, all_contig_fragments, 0);

//			visit_curr_node(contig);

			// Count total edges
			int total_edge_count = 0;
			struct linked_node* to = contig->curr_node->toNodes;

			while (to != NULL) {
				total_edge_count = total_edge_count + to->node->frequency;
				to = to->next;
			}

			double log10_total_edge_count = log10(total_edge_count);

			// Move current contig to next "to" node.
			struct linked_node* to_linked_node = contig->curr_node->toNodes;

			contig->curr_node = to_linked_node->node;
			paths_from_root++;

			// If there are multiple "to" nodes, branch the contig and push on stack
			to_linked_node = to_linked_node->next;

			while (to_linked_node != NULL) {
				struct contig* contig_branch = copy_contig(contig, all_contig_fragments);
				contig_branch->curr_node = to_linked_node->node;
				contig_branch->score = contig_branch->score + log10(contig_branch->curr_node->frequency) - log10_total_edge_count;
				contigs.push(contig_branch);
				to_linked_node = to_linked_node->next;
				paths_from_root++;
			}

			contig->score = contig->score + log10(contig->curr_node->frequency) - log10(total_edge_count);
		}

		if (contig_count >= max_contigs) {
			status = TOO_MANY_CONTIGS;
		}

		if (paths_from_root >= max_paths_from_root) {
			status = TOO_MANY_PATHS_FROM_ROOT;
		}
	}

	while (contigs.size() > 0) {
		struct contig* contig = contigs.top();
		contigs.pop();
		free_contig(contig);
	}

	while (popped_contigs.size() > 0) {
		struct contig* contig = popped_contigs.top();
		popped_contigs.pop();
		free_contig(contig);
	}

	for (vector<char*>::iterator it=all_contig_fragments.begin(); it != all_contig_fragments.end(); ++it) {
		free(*it);
	}

	return status;
}

int processed_nodes = 0;

//TODO: Use thread pool instead of spawning threads
void* worker_thread(void* t) {

	struct linked_node* root = (struct linked_node*) t;

	while (root != NULL) {

		struct node* source = root->node;

		if (score_seq(source->kmer, MIN_ROOT_HOMOLOGY_SCORE)) {

			int contig_count = 0;
			const char* prefix = "foo";
			int max_paths_from_root = 500000000;
			int max_contigs = 50000000;
			char stop_on_repeat = false;
			char shadow_mode = false;
			char* contig_str = NULL;
			int status = build_contigs(source, contig_count, prefix, max_paths_from_root, max_contigs, stop_on_repeat,
					shadow_mode, contig_str);

			if (status != OK) {
				fprintf(stderr, "Status: %d\n", status);
				fflush(stderr);
				exit(status);
			}

			processed_nodes += 1;
			if ((processed_nodes % 100) == 0) {
				fprintf(stderr, "Processed roots: %d\n", processed_nodes);
			}
		} else {
			fprintf(stderr, "Skipping root: ");
			print_kmer(source->kmer);
			fprintf(stderr, "\n");
		}

		root = root->next;
	}

	pthread_mutex_lock(&running_thread_mutex);
	running_threads -= 1;
	pthread_mutex_unlock(&running_thread_mutex);
}

void dump_graph(dense_hash_map<const char*, struct node*, my_hash, eqstr>* nodes, const char* filename) {

	FILE* fp = fopen(filename, "w");

	// Output edges
	fprintf(fp, "digraph vdjician {\n// Edges\n");
	for (dense_hash_map<const char*, struct node*, my_hash, eqstr>::const_iterator it = nodes->begin();
				 it != nodes->end(); ++it) {

		const char* key = it->first;
		node* curr_node = it->second;

		struct linked_node* to_node = curr_node->toNodes;

		while (to_node != NULL) {
			// Copy current node to output buf
			char output[1024];
			memset(output, 0, 1024);
			strncpy(output, curr_node->kmer, kmer_size);
			int idx = kmer_size;
			strncpy(&(output[idx]), " -> ", 4);
			idx += 4;

			strncpy(&(output[idx]), to_node->node->kmer, kmer_size);
			idx += kmer_size;
			output[idx] = ';';

			fprintf(fp, "\t%s\n", output);
			to_node = to_node->next;
		}
	}

	// Output vertices
	fprintf(fp, "// Vertices\n");
	for (dense_hash_map<const char*, struct node*, my_hash, eqstr>::const_iterator it = nodes->begin();
				 it != nodes->end(); ++it) {

		const char* key = it->first;
		node* curr_node = it->second;

		char output[1024];
		memset(output, 0, 1024);
		strncpy(output, curr_node->kmer, kmer_size);
		int idx = kmer_size;
		strncpy(&(output[idx]), ";", 1);

		fprintf(fp, "\t%s\n", output);
	}

	fprintf(fp, "}\n");

	fclose(fp);
}

linked_node* traceback_roots(linked_node* root) {

	dense_hash_set<const char*, vregion_hash, vregion_eqstr> contig_index;
	contig_index.set_empty_key(NULL);
	load_root_similarity_index(contig_index);

	linked_node* new_roots = NULL;
	linked_node* new_roots_ptr = NULL;

	dense_hash_set<char*, my_hash, eqstr> tracebacks;
	tracebacks.set_empty_key(NULL);

	while (root != NULL) {
		struct node* node = root->node;

		//
		// Walk backwards in graph in until there are no more nodes or
		// a fork in the graph
		int ctr = 0;  // Don't allow infinite loop
		while (node->fromNodes != NULL && node->fromNodes->next == NULL & ctr++ < 300) {
			node = node->fromNodes->node;
//			fprintf(stderr, "\t");
//			print_kmer(node->kmer);
//			fprintf(stderr, "\n");
		}

		fprintf(stderr, "Traceback dist: %d\n", ctr);

		if (tracebacks.find(node->kmer) == tracebacks.end()) {

			if (has_vregion_homology(node->kmer, contig_index)) {
				fprintf(stderr, "ROOT_NODE:\t%d\t", node->frequency);
				print_kmer(node->kmer);
				fprintf(stderr, "\n");

				tracebacks.insert(node->kmer);
				if (new_roots == NULL) {
					new_roots = (linked_node*) calloc(1, sizeof(linked_node));
					new_roots_ptr = new_roots;
				} else {
					new_roots_ptr->next = (linked_node*) calloc(1, sizeof(linked_node));
					new_roots_ptr = new_roots_ptr->next;
				}

				new_roots_ptr->node = node;
				new_roots_ptr->next = NULL;

			} else {
				fprintf(stderr, "FILTERED_NODE:\t%d\t", node->frequency);
				print_kmer(node->kmer);
				fprintf(stderr, "\n");
			}
		}

		root = root->next;
	}

	fprintf(stderr, "New roots size: %d\n", tracebacks.size());

	return new_roots;
}

void cleanup(struct linked_node* linked_nodes) {
	struct linked_node* ptr = linked_nodes;
	while (ptr != NULL) {
		struct linked_node* next = ptr->next;
		free(ptr);
		ptr = next;
	}
}

void cleanup(dense_hash_map<const char*, struct node*, my_hash, eqstr>* nodes, struct struct_pool* pool) {

	// Free linked lists
	for (dense_hash_map<const char*, struct node*, my_hash, eqstr>::const_iterator it = nodes->begin();
	         it != nodes->end(); ++it) {
		struct node* node = it->second;

		if (node != NULL) {
			cleanup(node->toNodes);
			cleanup(node->fromNodes);
			free(node);
		}
	}
}

char* assemble(const char* input,
			   const char* unaligned_input,
			  const char* output,
			  const char* prefix,
			  int truncate_on_repeat,
			  int max_contigs,
			  int max_paths_from_root,
			  int input_read_length,
			  int input_kmer_size) {


	fprintf(stderr, "Assembling...\n");
	read_length = input_read_length;

	kmer_size = input_kmer_size;

	struct struct_pool* pool = NULL;
	dense_hash_map<const char*, struct node*, my_hash, eqstr>* nodes = new dense_hash_map<const char*, struct node*, my_hash, eqstr>();
	nodes->set_empty_key(NULL);

	long startTime = time(NULL);
	fprintf(stderr, "Assembling: -> %s\n", output);


	pthread_mutex_init(&running_thread_mutex, NULL);
	pthread_mutex_init(&contig_writer_mutex, NULL);

	struct linked_node* root_nodes = NULL;

	// TODO: Factor out to separate function
	// Code block here is used to allow pre_nodes to go out of scope and free memory.
	{
		sparse_hash_map<const char*, pre_node, my_hash, eqstr> pre_nodes;
		pre_nodes.set_deleted_key(NULL);

		print_status("PRE_PRE_GRAPH1");
		build_pre_graph(input, pre_nodes);
		print_status("PRE_PRE_GRAPH2");
		build_pre_graph(unaligned_input, pre_nodes);
		print_status("POST_PRE_GRAPH1");

		prune_pre_graph(pre_nodes);
		fprintf(stderr, "pre nodes after pruning: %d\n", pre_nodes.size());
		print_status("POST_PRUNE_PRE_GRAPH1");

		build_graph2(input, nodes, pool, 1, pre_nodes);

		print_status("POST_BUILD_GRAPH1");

		char isUnalignedRegion = !truncate_on_repeat;

		root_nodes = identify_root_nodes(nodes);

		build_graph2(unaligned_input, nodes, pool, 0, pre_nodes);
		print_status("POST_BUILD_GRAPH2");

		pre_nodes.clear();
		pre_nodes.resize(0);
	} // End pre_node block

	print_status("POST_GRAPH_BLOCK");

	fprintf(stderr, "Total nodes: %d\n", nodes->size());

	int status = -1;

	if (nodes->size() >= MAX_NODES) {
		status = TOO_MANY_NODES;
		fprintf(stderr, "Graph too complex for region: %s\n", prefix);
	}

	root_nodes = traceback_roots(root_nodes);

	print_status("POST_ROOT_TRACEBACK");

	dump_graph(nodes, "graph.dot");


	// The initial set of root nodes are used as markers (or breadcrumbs)
	// We start from a marker and follow the trail back to the true source nodes
//	root_nodes = get_roots_from_marker_nodes(root_nodes);

//	if (status != TOO_MANY_NODES) {
//		root_nodes = identify_root_nodes(nodes);
//	}

	int contig_count = 0;
	char truncate_output = 0;

	char* contig_str = (char*) malloc(MAX_TOTAL_CONTIG_LEN);
	memset(contig_str, 0, MAX_TOTAL_CONTIG_LEN);


	pthread_t threads[MAX_THREADS];
	int num_threads = 0;
	running_threads = 0;

	int num_roots = 0;
	pthread_mutex_init(&running_thread_mutex, NULL);
	pthread_mutex_init(&contig_writer_mutex, NULL);

	linked_node* root_link = NULL;

	long ts = time(NULL);
	long te = time(NULL);

	while (root_nodes != NULL) {

		while (running_threads >= MAX_RUNNING_THREADS) {
			te = time(NULL);
			if (te-ts > 300) {
				print_status("STATUS_UPDATE");
				ts = te;
			}
			// Sleep for 10 milliseconds
			usleep(10*1000);
		}

		int ROOT_NODES_PER_THREADS = 10;

		int root_count = 1;
		linked_node* next_root_start = root_nodes;
		linked_node* last = root_nodes;
		while (root_nodes != NULL && root_count < ROOT_NODES_PER_THREADS) {
			root_nodes = root_nodes->next;
			if (root_nodes != NULL) {
				last = root_nodes;
			}
			num_roots++;
			root_count += 1;
		}

		root_nodes = last->next;
		last->next = NULL;

		running_threads += 1;
		int ret = pthread_create(&(threads[num_threads]), NULL, worker_thread, next_root_start);


		if (ret != 0) {
			fprintf(stderr, "Error creating thread 1: %d\n", ret);
			fflush(stdout);
			exit(-1);
		}

		fprintf(stderr, "Running threads: %d\n", running_threads);

		num_threads++;

		if ((num_roots % 10) == 0) {
			fprintf(stderr, "Processed %d root nodes\n", num_roots);
			fprintf(stderr, "Num candidate contigs: %d\n", vjf_windows.size());
			fprintf(stderr, "Window candidate size: %d\n", vjf_window_candidates.size());
			fflush(stdout);
//			print_status("STATUS_UPDATE");

		}
	}

	// Wait for all threads to complete
	while (running_threads > 0) {
		te = time(NULL);
		if (te-ts > 300) {
			print_status("STATUS_UPDATE");
			ts = te;
		}
		// Sleep for 10 milliseconds
		usleep(10*1000);

	}

	print_status("THREADS_DONE");

	pthread_mutex_destroy(&running_thread_mutex);
	pthread_mutex_destroy(&contig_writer_mutex);

	// Write windows to disk
	fprintf(stderr, "Writing windows to disk\n");
	output_windows();

	print_status("PRE_CLEANUP");
	cleanup(nodes, pool);
	delete nodes;
	print_status("POST_CLEANUP");

	long stopTime = time(NULL);

	if (kmer_size != input_kmer_size) {
		fprintf(stderr, "What!!?? %d : %d\n", kmer_size, input_kmer_size);
	}
	assert(kmer_size == input_kmer_size);

	print_status("FINIS");

	fprintf(stderr, "Done assembling(%ld): %s, %d\n", (stopTime-startTime), output, contig_count);

	if (status == OK || status == TOO_MANY_PATHS_FROM_ROOT) {
		return contig_str;
	} else if (status == STOPPED_ON_REPEAT) {
		strcpy(contig_str, "<REPEAT>");
		return contig_str;
	} else {
		strcpy(contig_str, "<ERROR>");
		return contig_str;
	}
}

char* load_file(const char* filename) {
	FILE    *infile;
	char    *buffer;
	long    numbytes;

	printf("Loading: %s\n", filename);
	fflush(stdout);

	infile = fopen(filename, "r");
	fseek(infile, 0L, SEEK_END);
	numbytes = ftell(infile);
	printf("numbytes: %ld\n", numbytes);
	fflush(stdout);
	buffer = (char*) malloc(numbytes+1);
	memset(buffer, 0, numbytes+1);
	fseek(infile, 0L, SEEK_SET);
	fread(buffer, sizeof(char), numbytes, infile);
	fclose(infile);
	return buffer;
}

int main(int argc, char* argv[]) {

	print_status("START");

	char* input_file = argv[1];
	char* unaligned_input_file = NULL;

    min_node_freq = 5;
    min_base_quality = 150;

	min_node_freq = atoi(argv[2]);
	min_base_quality = atoi(argv[3]);
	if (min_base_quality >= MAX_QUAL_SUM) {
		min_base_quality = MAX_QUAL_SUM-1;
	}

	int read_len = atoi(argv[4]);
	read_length = read_len;

	MIN_CONTIG_SCORE = atof(argv[5]);

	MAX_RUNNING_THREADS = atoi(argv[6]);

	if (argc >= 8) {
		unaligned_input_file = argv[7];
	}

	char* vjf_v_file = argv[8];
	char* vjf_j_file = argv[9];
	int vjf_max_dist = atoi(argv[10]);
	int vjf_min_win = atoi(argv[11]);
	int vjf_max_win = atoi(argv[12]);
	char vjf_j_conserved = argv[13][0];
	int vjf_window_span = atoi(argv[14]);
	int vjf_j_extension = atoi(argv[15]);
	char* vdj_fasta = argv[16];
	char* v_region = argv[17];
	char* c_region = argv[18];
	INSERT_LEN = atoi(argv[19]);
	FILTER_READ_FLOOR = atoi(argv[20]);
	int kmer = atoi(argv[21]);
	ROOT_SIMILARITY_FILE = argv[22];
	VREGION_KMER_SIZE = atoi(argv[23]);
	MIN_ROOT_HOMOLOGY_SCORE = atoi(argv[24]);

	fprintf(stderr, "mnf: %d\n", min_node_freq);
	fprintf(stderr, "mbq: %d\n", min_base_quality);
	fprintf(stderr, "kmer: %d\n", kmer);

	// Initialize seq scoring for root node evalulation
	score_seq_init(kmer, 1000, ROOT_SIMILARITY_FILE);

	vjf_windows.set_empty_key(NULL);
	vjf_window_candidates.set_empty_key(NULL);
	vjf_init(vjf_v_file, vjf_j_file, vjf_max_dist, vjf_min_win, vjf_max_win,
			vjf_j_conserved, vjf_window_span, vjf_j_extension);

	print_status("POST_VJF_INIT");

	char* bam_file = input_file;
	char* input = NULL;
	char* unaligned_input = NULL;
	fprintf(stderr, "Extracting reads...\n");
	fflush(stdout);
	extract(bam_file, vdj_fasta, v_region, c_region, input, unaligned_input);
	fprintf(stderr, "Read extract done...\n");
	fflush(stdout);

	print_status("POST_READ_EXTRACT");

        char* output = assemble(
		input,
		unaligned_input,
		"",
                "foo",
                false,
                50000000,
                500000000,
                read_len,
                kmer);
}



