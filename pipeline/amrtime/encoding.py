import os
import sys
import subprocess
import pandas as pd
import numpy as np
from amrtime import parsers
import math
import itertools
import re

class Homology():
    """
    Generate a read encoding
    """
    def __init__(self, simulated_reads, reference_db, tool):

        self.reads = simulated_reads
        self.db = reference_db

        if tool == 'DIAMOND':
            if not os.path.exists('training_data/diamond.out6') :
                self.alignment_fp = self.run_diamond_alignment(self.reads, self.db)
            else:
                self.alignment_fp = 'training_data/diamond.out6'
        elif tool == 'MMSEQS2':
            if not os.path.exists('training_data/mmseqs.out6') :
                self.alignment_fp = self.run_alignment(self.reads, self.db)
            else:
                self.alignment_fp = 'training_data/mmseqs.out6'

    def run_diamond_alignment(self, reads, db):
        """
        Perform a DIAMOND BLASTX search to gather homology data
        """
        # build database if it doesn't exist
        if not os.path.exists(db + '.dmnd'):
            subprocess.check_call('diamond makedb --in {0} --db {0}'.format(db),
        shell=True)

        # run alignment
        subprocess.check_call('diamond blastx --db {} --out training_data/diamond.out6 --outfmt 6 --threads 2 --query {} --more-sensitive'.format(db, reads), shell=True)
        return 'training_data/diamond.out6'

    def run_mmseqs_alignment(self, reads, db):
        subprocess.check_call('mmseqs easy-search {} {} training_data/mmseqs.out6 /tmp'.format(db, reads), shell=True)


    def encode(self, card, metric, norm=False, dissimilarity=False):
        alignment_fh = open(self.alignment_fp)
        reads_fh = open(self.reads)

        # build reference to get the correct row for each AMR family
        family_to_field = {family: ix for ix, family in enumerate(set(card.aro_to_gene_family.values()))}
        field_to_family = {v: k for k, v in family_to_field.items()}

        # build the same for the aros
        aro_to_field = {aro: ix for ix, aro in enumerate(set(card.aro_to_gene_family.keys()))}
        field_to_aro = {v: k for k, v in aro_to_field.items()}


        # read input fastq and initialise an empty vectors for each read
        # and store in a dictionary of read_acc: vector
        gene_family_encoding = {}
        aro_encoding = {}
        read_ixs = []
        for ix, read in enumerate(reads_fh):
            if ix % 4 == 0:
                read_acc = read.strip().replace('@gb', 'gb')
                read_ixs.append(read_acc)
                # initalise the gene family and aro similarity vectors
                # i.e. x_j for j is the similarity to the gene family
                # or aro of interest
                family_sim_vector = np.zeros(len(family_to_field))
                aro_sim_vector = np.zeros(len(aro_to_field))

                gene_family_encoding.update({read_acc : family_sim_vector})
                aro_encoding.update({read_acc: aro_sim_vector})


        # read the alignment file and store the top blast score per family
        # for each read
        if metric == 'bitscore':
            out_field = 11
        elif metric == 'evalue':
            out_field = 10
        elif metric == 'pident':
            out_field = 2

        scores = {}
        for alignment in alignment_fh:
            alignment = alignment.strip().split('\t')
            query_acc = alignment[0]
            alignment_aro = alignment[1].split('|')[2]
            score = float(alignment[out_field])

            if metric == 'evalue':
                score = math.log(score)

            # convert ARO to AMR family and get the index in the vector
            # for that family
            amr_family = card.aro_to_gene_family[alignment_aro]
            family_field = family_to_field[amr_family]
            aro_field = aro_to_field[alignment_aro]

            # if this bitscore is greater than the already highest for that
            # read and family
            if metric == 'evalue':
                if score < gene_family_encoding[query_acc][family_field]:
                    gene_family_encoding[query_acc][family_field] = score
                    aro_encoding[query_acc][aro_field] = score
            else:
                if score > gene_family_encoding[query_acc][family_field]:
                    gene_family_encoding[query_acc][family_field] = score
                    aro_encoding[query_acc][aro_field] = score

        alignment_fh.close()
        reads_fh.close()

        # create family encoding
        family_df = pd.DataFrame(gene_family_encoding)
        family_df = family_df.transpose()
        # rename the columns with the gene families
        family_index = [field_to_family[x] for x in range(len(field_to_family))]
        family_df.columns = family_index
        # to reorder the encoding correctly
        family_df = family_df.loc[read_ixs]


        # create aro encoding
        aro_df = pd.DataFrame(aro_encoding)
        aro_df = aro_df.transpose()
        # rename the columns with the correct ARO
        aro_index = [field_to_aro[x] for x in range(len(field_to_aro))]
        aro_df.columns = aro_index
        # to reorder the encoding correctly
        aro_df = aro_df.loc[read_ixs]

        # normalise bitscores
        if norm and metric == 'bitscore':
            card.calculate_maximum_bitscores_per_family()
            for family in family_df:
                family_df[family] = family_df[family] / card.max_family_bitscores[family]

            card.calculate_maximum_bitscores_per_aro()
            for aro in aro_df:
                aro_df[aro] = aro_df[aro] / card.max_aro_bitscores[aro]
        elif norm and metric != 'bitscore':
            print("Can't normalise non bitscore metrics currently, must set metric to bitscore")
            sys.exit(1)

        # normalise and calculate dissimilarity matrices
        if dissimilarity and norm:
            family_df = family_df.applymap(lambda x: 1-x)
            family_df = family_df.fillna(0)
            aro_df = aro_df.applymap(lambda x: 1-x)
            aro_df = aro_df.fillna(0)
        elif dissimilarity and not norm:
            print("Can't create dissimilarity matrix for non-normalised bitscores, must set dissimilarity and norm to True")
            sys.exit(1)

        return family_df, aro_df


class Kmer():
    def __init__(self, metagenome_fp, k):
        self.metagenome_fp = metagenome_fp
        self.k = k

    def encode(self):
        tnf = ["".join(x) for x in itertools.product(['A', 'T', 'G', 'C', 'N'], repeat=self.k)]
        tnf_encoder = {v:k for k,v in enumerate(tnf)}

        X = []
        with open(self.metagenome_fp) as fh:
            for ix, line in enumerate(fh):
                if ix % 4 == 1:
                    clean_seq = re.sub("[M,X,R,S,Y,K]", 'N', line.strip())
                    self.seq = clean_seq
                    encoded_seq = self.read_encode(clean_seq, tnf_encoder)
                    X.append(encoded_seq)
        return np.vstack(X)

    def read_encode(self, seq, tnf):
        """
        k-mer decomposition might be simplest although might have to be
        done at the file level in order to represent all k-mers
        """
        encoded_vec = np.zeros(len(tnf))
        for tetranucleotide in self.window(self.k):
            encoded_vec[tnf[tetranucleotide]] += 1
        return encoded_vec

    def window(self, window_size):
        "Returns a sliding window (of width w) over data from the iterable"
        "   s -> (s0,s1,...s[w-1]), (s1,s2,...,sw), ...                   "
        it = iter(self.seq)
        result = tuple(itertools.islice(it, window_size))
        if len(result) == window_size:
            yield "".join(result)
        for elem in it:
            result = result[1:] + (elem,)
            yield "".join(result)

# dna2vec kmer embedding
class KMer_embedding():
    pass
