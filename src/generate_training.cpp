#include <fstream>
#include <iostream>
#include <string>
#include <algorithm>

#include <seqan/seq_io.h>
#include <seqan/bam_io.h>
#include <seqan/arg_parse.h>
#include <seqan/sequence.h>

#include "AMRtimeConfig.h"
#include "generate_training.h"

//using namespace seqan;

#define MIN_OVERLAP 50


std::ostream &operator<<(std::ostream &os, AMR_annotation const &m) { 
    // Overload the output stream operator for annotation class
    // to dump the attributes
    return os << " contig: " << m.contig << " aro: " << m.aro 
            << " start: " << m.start << " end: " << m.end << " strand: " << m.strand;
}

seqan::ArgumentParser::ParseResult parse_command_line(Options& options, 
                                                      int argc,
                                                      char** argv){
    // Build argument parser and store parsed arguments 
    // in an instance of the Options class
    
    seqan::ArgumentParser parser("generate_training");

    setShortDescription(parser, "Synthetic Metagenomes Generator");

    setVersion(parser, AMRtime_VERSION);

    addUsageLine(parser, "[\\fIOPTIONS] \\fIGENOME_LIST\f \\fIANNOTATION_LIST\f \\fIABUNDANCE_LIST\f");

    addDescription(
            parser,
            "Tool to generate synthetic metagenomes at specified coverage"
            " and relative abundances from annotated genomes.");

    addArgument(parser, seqan::ArgParseArgument(
        seqan::ArgParseArgument::STRING, "genomes"));

    addArgument(parser, seqan::ArgParseArgument(
        seqan::ArgParseArgument::STRING, "annotations"));

    addArgument(parser, seqan::ArgParseArgument(
        seqan::ArgParseArgument::STRING, "abundances"));

    addOption(parser, seqan::ArgParseOption(
        "c", "coverage", "Required coverage for metagenome",
        seqan::ArgParseOption::INTEGER, "coverage"));
    setDefaultValue(parser, "coverage", 1);

    addOption(parser, seqan::ArgParseOption(
        "r", "read_length", "length of reads to simulate",
        seqan::ArgParseOption::INTEGER, "read_length"));
    setDefaultValue(parser, "read_length", 150);

    addOption(parser, seqan::ArgParseOption(
        "o", "output_name", "output file name",
        seqan::ArgParseOption::STRING, "output_name"));
    setDefaultValue(parser, "output_name", "output");

    seqan::ArgumentParser::ParseResult res = seqan::parse(parser, argc, argv);

    if (res != seqan::ArgumentParser::PARSE_OK){
        return res;
    }
    
    std::string temp;
    getArgumentValue(temp, parser, 0);
    options.genomes = split(temp, ',');

    getArgumentValue(temp, parser, 1);
    options.annotations = split(temp, ',');

    getArgumentValue(temp, parser, 2);
    std::vector<std::string> abundance_strings = split(temp, ',');
    for (uint32_t i=0; i<abundance_strings.size(); ++i) {
        uint32_t abundance = std::stoi(abundance_strings.at(i).c_str());
        options.relative_abundances.push_back(abundance);
    } 

    bool length_ok = (options.genomes.size() == options.annotations.size() && \
            options.annotations.size () == options.relative_abundances.size());
    if (!length_ok) {
        std::cerr << "ERROR: You must provide the same number of genomes, annotations "
                  << "(and relative abundances if specified)" << std::endl;
        return seqan::ArgumentParser::PARSE_ERROR;
    }

    getOptionValue(options.coverage, parser, "coverage");
    getOptionValue(options.read_length, parser, "read_length");
    getOptionValue(options.output_name, parser, "output_name");

    return seqan::ArgumentParser::PARSE_OK;
}

int main(int argc, char *argv[]){
    // Create simulated metagenome and label the reads

    // Get command line arguments
    Options options;
    seqan::ArgumentParser::ParseResult res = parse_command_line(options, 
                                                                argc, 
                                                                argv);

    if (res != seqan::ArgumentParser::PARSE_OK){
        return res == seqan::ArgumentParser::PARSE_ERROR;
    }
    
    // Generate the synthetic metagenome fasta file
    // i.e. concatenate the input genomes and 'amplify'
    // them up to the required copy number to satisfy relative
    // abundances
    std::cout << "Creating Synthetic Metagenome Fasta: ";
    std::vector<std::string>::iterator it;
    for(it = options.genomes.begin(); it != options.genomes.end(); ++it) {
        std::cout << *it << " ";
    }
    std::cout << std::endl << std::endl;

    std::string metagenome_fp = prepare_metagenome(options.genomes, 
                                                   options.relative_abundances,
                                                   options.output_name);

    
    // Determine how many reads we need to create to get the approximate
    // coverage specified
    uint32_t read_number = estimate_read_depth(metagenome_fp,
                                               options.coverage,
                                               options.read_length);
    
    // Simulate the reads themselves using mason and a system call
    std::cout << "Simulating Illumina Reads: " << options.read_length << "bp " 
        << read_number << " reads" << std::endl << std::endl;
    std::stringstream ss;
    std::string simulated_sam_fp = options.output_name + ".sam";
    
    // run mason with no sequencing errors for now
    ss << "mason_simulator -ir " << metagenome_fp << " -n " << read_number << " -oa " 
       << simulated_sam_fp << " -o " << options.output_name + ".fq" 
       << " --illumina-read-length " << options.read_length 
       << " --illumina-prob-insert 0 --illumina-prob-deletion 0 --illumina-prob-mismatch-scale 0"
       << " --illumina-prob-mismatch 0 --illumina-prob-mismatch-begin 0 --illumina-prob-mismatch-end 0"
       << " 2> /dev/null" << std::endl;
       
       
    system(ss.str().c_str());
    
    std::cout << "Parsing GFF annotations: ";
    for(it = options.annotations.begin(); it != options.annotations.end(); ++it) {
        std::cout << *it << " ";
    }
    std::cout << std::endl << std::endl;

    std::vector<AMR_annotation> amr_annotations = read_amr_annotations(options.annotations);
    
    std::cout << "Creating labels: " << options.output_name + ".labels" << std::endl;
    create_labels(amr_annotations, simulated_sam_fp, options.output_name);

    return 0;
}


void create_labels(std::vector<AMR_annotation> annotations, std::string sam_fp, 
                   std::string output_name){
    
    // Open input file, BamFileIn can read SAM and BAM files.
    seqan::BamFileIn bamFileIn;
    if (!open(bamFileIn, seqan::toCString(sam_fp))) {
        std::cerr << "ERROR: Could not open " << sam_fp << std::endl;
        std::exit(1);
    }
    // Open output file, BamFileOut accepts also an ostream and a format tag.
    
            
    std::ofstream labels_fh;
    labels_fh.open (output_name + ".labels");
    
    try {
        // Copy header.
        seqan::BamHeader header;
        readHeader(header, bamFileIn);

        // read header into queriable context
        seqan::BamAlignmentRecord bamRecord;
        typedef seqan::FormattedFileContext<seqan::BamFileIn, void>::Type TBamContext;
        TBamContext const & bamContext = context(bamFileIn);


        
        // for every read generated
        while (!atEnd(bamFileIn)) {
            
            readRecord(bamRecord, bamFileIn);
            
            // create a vector of labels
            std::vector<std::string> labels {};
            
            // by checking all the annotations
            for (auto &annotation : annotations){
                
                // only check when the annotation's contig is the same as the reads
                if (annotation.contig == seqan::toCString(contigNames(bamContext)[bamRecord.rID])) {
                    
                    bool both_pos = annotation.strand == '+' and not hasFlagRC(bamRecord);
                    bool both_neg = annotation.strand == '-' and hasFlagRC(bamRecord);
                    
                    // both on the same strand
                    if (both_pos or both_neg) {
                        
                        int32_t overlap = range_overlap(bamRecord.beginPos, 
                                                        bamRecord.beginPos + length(bamRecord.seq),
                                                        annotation.start, 
                                                        annotation.end);
                                                        
                        if (overlap > MIN_OVERLAP) {
                            labels.push_back(annotation.aro);
                        }
                    }
                }
            }
            // sort and find unique AROs in labels due gff duplication issue wit RGI
            std::sort(labels.begin(), labels.end());
            labels.erase(std::unique(labels.begin(), labels.end()), labels.end());
            
            // print the labels
            if (labels.size() == 0) {
                labels_fh << "NONE" << std::endl;
            }
            else {
                //labels_fh << bamRecord.qName << ": ";
                for (std::vector<std::string>::const_iterator i = labels.begin(); i != labels.end(); ++i){
                    labels_fh << *i << ' ';
                }
                labels_fh << std::endl;
            }
        }
    }
    
    
    catch (seqan::Exception const & e) {
        std::cout << "ERROR: " << e.what() << std::endl;
        std::exit(1);
    }
    
    labels_fh.close();
    
}

int32_t range_overlap(uint32_t annot_start, uint32_t annot_end, 
                       uint32_t read_loc_start, uint32_t read_loc_end){
 
    std::vector<uint32_t> annot_range;
    for (uint32_t i = annot_start; i <= annot_end; ++i) {
        annot_range.push_back(i);
    }

    std::vector<uint32_t> read_loc_range;
    for (uint32_t i = read_loc_start; i <= read_loc_end; ++i) {
        read_loc_range.push_back(i);
    }
    
    std::vector<uint32_t> range_intersection;
 
    std::set_intersection(annot_range.begin(), annot_range.end(),
                          read_loc_range.begin(), read_loc_range.end(),
                          std::back_inserter(range_intersection));
    
    // overlap is 1 too big...
    return range_intersection.size() - 1;

}

std::vector<AMR_annotation> read_amr_annotations(std::vector<std::string> gff_list) {

    std::vector<AMR_annotation> annotations;
    
    for (auto &gff_fp : gff_list){

        seqan::GffFileIn gffFileIn;
        if (!open(gffFileIn, seqan::toCString(gff_fp))) {
            std::cerr << "ERROR: Could not open file: " << gff_fp << std::endl;
            std::exit(1);
        }

        seqan::GffRecord gffRecord;
        std::string aro;

        while (!atEnd(gffFileIn)) {
            try {
                readRecord(gffRecord, gffFileIn);

                aro = "";
                
                for (uint32_t i = 0; i < length(gffRecord.tagValues[1]); ++i) {
                    if (gffRecord.tagValues[1][i] == ','){
                        break;
                    }
                    aro.push_back(gffRecord.tagValues[1][i]);
                }
                
                // truncate the gff suffix from the contig name
                std::string contig_name = seqan::toCString(gffRecord.ref);
                contig_name = contig_name.substr(0, contig_name.find("_"));
                
                // build annotation data together
                AMR_annotation annotation {contig_name,
                                           aro,
                                           gffRecord.beginPos,
                                           gffRecord.endPos,
                                           gffRecord.strand};

                annotations.push_back(annotation);

            }
            // necessary for \r and \n endline chars?
            catch (seqan::ParseError const & e) {
                break;
            }
        }
   }
   return annotations;
}

std::string prepare_metagenome(std::vector<std::string> genome_list,
                               std::vector<uint32_t> abundance_list,
                               std::string output_name){
    // Copy the genomes up to necessary numbers into the artifical
    // metagenome contigs

    seqan::StringSet<seqan::CharString> ids;
    seqan::StringSet<seqan::Dna5String> seqs;

    // read each genome in list and 'amplify' for relative abundance
    for(uint32_t genome_ix = 0; genome_ix < genome_list.size(); ++genome_ix){

        seqan::SeqFileIn seqFileIn;
        if (!open(seqFileIn, seqan::toCString(genome_list[genome_ix]))) {
            std::cerr << "ERROR: Could not open file: " << genome_list[genome_ix] << std::endl;
            exit(1);
        }

        seqan::StringSet<seqan::CharString> temp_ids;
        seqan::StringSet<seqan::Dna5String> temp_seqs;

        readRecords(temp_ids, temp_seqs, seqFileIn);

        // append the sequences as many times as the relative abundance
        // implies i.e. if its 3, copy the sequences 3x into the master
        // metagenome fasta
        // append the copy number to the id as a suffix to prevent sam parsing errors
        for(uint32_t copy_number = 0; copy_number < abundance_list[genome_ix]; ++copy_number){
            append(ids, temp_ids);
            append(seqs, temp_seqs);
        }

    }

    // dump artificial metagenome to single fasta file
    std::string metagenome_fp = output_name + "_metagenome.fasta";
    seqan::SeqFileOut seqFileOut;
    if (!open(seqFileOut, seqan::toCString(metagenome_fp))) {
            std::cerr << "ERROR: Could not open file: temp_metagenome.fasta" << std::endl;
            exit(1);
    }

    writeRecords(seqFileOut, ids, seqs);

    return metagenome_fp;

}

uint32_t count_nucleotides(std::string combined_genome_fp){
    // read a fasta and count the nucleotides
    int nt_count = 0;

    seqan::SeqFileIn seqFileIn;
    if (!open(seqFileIn, seqan::toCString(combined_genome_fp))) {
        std::cerr << "ERROR: Could not open file: " << combined_genome_fp << std::endl;
        exit(1);
    }

    seqan::CharString id;
    seqan::Dna5String seq;
    while (!atEnd(seqFileIn)){
        readRecord(id, seq, seqFileIn);
        nt_count += length(seq);
    }

    return nt_count;
}


uint32_t estimate_read_depth(std::string combined_genome_fp,
                             uint32_t coverage_fold,
                             uint32_t read_length){
    // Calculate the necessary read number for a specified read depth

    uint32_t nt_count = count_nucleotides(combined_genome_fp);


    double read_number_float = (coverage_fold * nt_count) / read_length;

    uint32_t read_number = static_cast<int>(read_number_float);

    return read_number;
}

std::vector<std::string> split(std::string str, char delimiter) {
    // split a string on a specific delimiter
    std::vector<std::string> split_string;
    std::stringstream ss(str);
    std::string fragment;

    while(std::getline(ss, fragment, delimiter)){
        split_string.push_back(fragment);
    }

    return split_string;
}
