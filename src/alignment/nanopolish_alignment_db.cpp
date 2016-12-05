//---------------------------------------------------------
// Copyright 2015 Ontario Institute for Cancer Research
// Written by Jared Simpson (jared.simpson@oicr.on.ca)
//---------------------------------------------------------
//
// nanopolish_alignment_db -- abstraction for working
// with sets of reads/events aligned to a reference genome
//
#include <assert.h>
#include <algorithm>
#include "nanopolish_alignment_db.h"
#include "htslib/faidx.h"
#include "htslib/hts.h"
#include "htslib/sam.h"
#include "nanopolish_methyltrain.h"

// Various file handle and structures
// needed to traverse a bam file
struct BamHandles
{
    htsFile* bam_fh;
    bam1_t* bam_record;
    hts_itr_t* itr;
};

AlignmentDB::AlignmentDB(const std::string& reads_file,
                         const std::string& reference_file,
                         const std::string& sequence_bam,
                         const std::string& event_bam,
                         bool calibrate_reads) :
                            m_reference_file(reference_file),
                            m_sequence_bam(sequence_bam),
                            m_event_bam(event_bam),
                            m_fast5_name_map(reads_file),
                            m_calibrate_on_load(calibrate_reads)
{
    _clear_region();
}

AlignmentDB::~AlignmentDB()
{
    _clear_region();
}

std::string AlignmentDB::get_reference_substring(const std::string& contig,
                                                 int start_position,
                                                 int stop_position) const
{
    assert(m_region_contig == contig);
    if(m_region_start > start_position || m_region_end < stop_position) {
        fprintf(stderr, "[alignmentdb] error: requested coordinates "
                "[%d %d] is outside of region boundary [%d %d]\n", 
                start_position, stop_position, m_region_start, m_region_end);
        exit(EXIT_FAILURE);
    }

    return m_region_ref_sequence.substr(start_position - m_region_start, stop_position - start_position + 1);
}


std::vector<std::string> AlignmentDB::get_read_substrings(const std::string& contig,
                                                          int start_position,
                                                          int stop_position) const
{
    assert(m_region_contig == contig);
    assert(m_region_start <= start_position);
    assert(m_region_end >= stop_position);

    std::vector<std::string> out;
    for(size_t i = 0; i < m_sequence_records.size(); ++i) {
        const SequenceAlignmentRecord& record = m_sequence_records[i];
        if(record.aligned_bases.empty())
            continue;

        int r1, r2;
        bool bounded = _find_by_ref_bounds(record.aligned_bases,
                                           start_position,
                                           stop_position,
                                           r1,
                                           r2);

        if(bounded) {
            out.push_back(record.sequence.substr(r1, r2 - r1 + 1));
        }
    }
    return out;
}

std::vector<HMMInputData> AlignmentDB::get_event_subsequences(const std::string& contig,
                                                              int start_position,
                                                              int stop_position) const
{
    assert(m_region_contig == contig);
    assert(m_region_start <= start_position);
    assert(m_region_end >= stop_position);

    std::vector<HMMInputData> out;
    for(size_t i = 0; i < m_event_records.size(); ++i) {
        const EventAlignmentRecord& record = m_event_records[i];
        if(record.aligned_events.empty()) {
            continue;
        }

        if(!record.sr->has_events_for_strand(record.strand)) {
            continue;
        }

        HMMInputData data;
        data.read = record.sr;
        data.anchor_index = -1; // unused
        data.strand = record.strand;
        data.rc = record.rc;
        data.event_stride = record.stride;
        
        int e1,e2;
        bool bounded = _find_by_ref_bounds(record.aligned_events, 
                                           start_position, 
                                           stop_position,
                                           e1,
                                           e2);
        if(bounded) {
            assert(e1 >= 0);
            assert(e2 >= 0);
            data.event_start_idx = e1;
            data.event_stop_idx = e2;
            out.push_back(data);
        }
    }

    return out;
}

std::vector<HMMInputData> AlignmentDB::get_events_aligned_to(const std::string& contig,
                                                             int position) const
{
    assert(m_region_contig == contig);
    assert(m_region_start <= position);
    assert(m_region_end >= position);

    std::vector<HMMInputData> out;
    for(size_t i = 0; i < m_event_records.size(); ++i) {
        const EventAlignmentRecord& record = m_event_records[i];
        if(record.aligned_events.empty()) {
            continue;
        }

        if(!record.sr->has_events_for_strand(record.strand)) {
            continue;
        }

        HMMInputData data;
        data.read = record.sr;
        data.anchor_index = -1; // unused
        data.strand = record.strand;
        data.rc = record.rc;
        data.event_stride = record.stride;
    
        AlignedPairConstIter start_iter;
        AlignedPairConstIter stop_iter;
        bool bounded = _find_iter_by_ref_bounds(record.aligned_events, position, position, start_iter, stop_iter);
        if(bounded && start_iter->ref_pos == position) {
            data.event_start_idx = start_iter->read_pos;
            data.event_stop_idx = start_iter->read_pos;
            out.push_back(data);
        }
    }
    return out;
}

std::vector<Variant> AlignmentDB::get_variants_in_region(const std::string& contig,
                                                         int start_position,
                                                         int stop_position,
                                                         double min_frequency,
                                                         int min_depth) const
{
    std::vector<Variant> variants;
    std::map<std::string, std::pair<Variant, int>> map;
    std::vector<int> depth(stop_position - start_position + 1, 0);

    for(size_t i = 0; i < m_sequence_records.size(); ++i) {
        const SequenceAlignmentRecord& record = m_sequence_records[i];
        if(record.aligned_bases.empty())
            continue;

        AlignedPairConstIter start_iter;
        AlignedPairConstIter stop_iter;
        _find_iter_by_ref_bounds(record.aligned_bases, start_position, stop_position, start_iter, stop_iter);

        // Increment the depth over this region
        int depth_start = start_iter->ref_pos;
        int depth_end = stop_iter == record.aligned_bases.end() ?
            record.aligned_bases.back().ref_pos : stop_iter->ref_pos;

        // clamp
        depth_start = std::max(depth_start, start_position);
        depth_end = std::min(depth_end, stop_position);

        for(; depth_start < depth_end; ++depth_start) {
            assert(depth_start >= start_position);
            assert(depth_start - start_position < depth.size());
            depth[depth_start - start_position]++;
        }

        //printf("[%zu] iter: [%d %d] [%d %d] first: %d last: %d\n", i, start_iter->ref_pos, start_iter->read_pos, stop_iter->ref_pos, stop_iter->read_pos,
        //            record.aligned_bases.front().ref_pos, record.aligned_bases.back().ref_pos);

        // Find the boundaries of a matching region
        while(start_iter != stop_iter) {
            // skip out-of-range
            int rp = start_iter->ref_pos;
            if(rp < start_position || rp > stop_position) {
                continue;
            }

            char rb = m_region_ref_sequence[start_iter->ref_pos - m_region_start];
            char ab = record.sequence[start_iter->read_pos];

            bool is_mismatch = rb != ab;
            auto next_iter = start_iter + 1;

            bool is_gap = next_iter != stop_iter &&
                            (next_iter->ref_pos != start_iter->ref_pos + 1 ||
                                next_iter->read_pos != start_iter->read_pos + 1);

            if(is_gap) {
                // advance the next iterator until a match is found
                while(next_iter != stop_iter) {
                    char n_rb = m_region_ref_sequence[next_iter->ref_pos - m_region_start];
                    char n_ab = record.sequence[next_iter->read_pos];
                    if(n_rb == n_ab) {
                        break;
                    }
                    ++next_iter;
                }
            }

            if(next_iter != stop_iter && (is_mismatch || is_gap)) {
                Variant v;
                v.ref_name = contig;
                v.ref_position = start_iter->ref_pos;

                size_t ref_sub_start = start_iter->ref_pos - m_region_start;
                size_t ref_sub_end = next_iter->ref_pos - m_region_start;
                v.ref_seq = m_region_ref_sequence.substr(ref_sub_start, ref_sub_end - ref_sub_start);
                v.alt_seq = record.sequence.substr(start_iter->read_pos, next_iter->read_pos - start_iter->read_pos);

                std::string key = v.key();
                auto iter = map.find(key);
                if(iter == map.end()) {
                    map.insert(std::make_pair(key, std::make_pair(v, 1)));
                } else {
                    iter->second.second += 1;
                }
            }
            start_iter = next_iter;
        }
    }

    for(auto iter = map.begin(); iter != map.end(); ++iter) {
        Variant& v = iter->second.first;
        size_t count = iter->second.second;
        size_t d = depth[v.ref_position - start_position];
        double f = (double)count / d;
        if(f >= min_frequency && (int)d >= min_depth) {
            v.add_info("BaseCalledReadsWithVariant", count);
            v.add_info("BaseCalledFrequency", f);
            variants.push_back(v);
        }
    }

    std::sort(variants.begin(), variants.end(), sortByPosition);
    return variants;
}

void AlignmentDB::load_region(const std::string& contig,
                              int start_position,
                              int stop_position)
{

    // load reference fai file
    faidx_t *fai = fai_load(m_reference_file.c_str());

    // Adjust end position to make sure we don't go out-of-range
    m_region_contig = contig;
    m_region_start = start_position;
    m_region_end = std::min(stop_position, faidx_seq_len(fai, contig.c_str()));

    // load the reference sequence for this region
    // its ok to use the unthreadsafe fetch_seq here since we have our own fai
    int fetched_len = 0;
    char* ref_segment = faidx_fetch_seq(fai, m_region_contig.c_str(), m_region_start, m_region_end, &fetched_len);
    m_region_ref_sequence = ref_segment;
    
    // load base-space alignments
    _load_sequence_by_region();

    // load event-space alignments
    _load_events_by_region();

    free(ref_segment);
    fai_destroy(fai);
}

void AlignmentDB::_clear_region()
{
    // Delete the SquiggleReads
    for(SquiggleReadMap::iterator iter = m_squiggle_read_map.begin();
        iter != m_squiggle_read_map.end(); ++iter) 
    {
        delete iter->second;
        iter->second = NULL;
    }

    m_squiggle_read_map.clear();
    m_sequence_records.clear();
    m_event_records.clear();

    m_region_contig = "";
    m_region_start = -1;
    m_region_end = -1;
}

BamHandles _initialize_bam_itr(const std::string& bam_filename,
                               const std::string& contig,
                               int start_position,
                               int stop_position)
{
    BamHandles handles;

    // load bam file
    handles.bam_fh = sam_open(bam_filename.c_str(), "r");
    assert(handles.bam_fh != NULL);

    // load bam index file
    std::string index_filename = bam_filename + ".bai";
    hts_idx_t* bam_idx = bam_index_load(index_filename.c_str());
    if(bam_idx == NULL) {
        fprintf(stderr, "Error: could not load the .bai index file for %s\n", bam_filename.c_str());
        fprintf(stderr, "Please run 'samtools index %s' before nanopolish\n", bam_filename.c_str());
        exit(EXIT_FAILURE);
    }

    // read the bam header to get the contig ID
    bam_hdr_t* hdr = sam_hdr_read(handles.bam_fh);
    int contig_id = bam_name2id(hdr, contig.c_str());
    
    // Initialize iteration
    handles.bam_record = bam_init1();
    handles.itr = sam_itr_queryi(bam_idx, contig_id, start_position, stop_position);

    hts_idx_destroy(bam_idx);
    bam_hdr_destroy(hdr);
    return handles;
}

void AlignmentDB::_load_sequence_by_region()
{
    assert(!m_region_contig.empty());
    assert(m_region_start >= 0);
    assert(m_region_end >= 0);

    BamHandles handles = _initialize_bam_itr(m_sequence_bam, m_region_contig, m_region_start, m_region_end);

    int result;
    while((result = sam_itr_next(handles.bam_fh, handles.itr, handles.bam_record)) >= 0) {
        SequenceAlignmentRecord seq_record;

        // copy sequence out of the record
        uint8_t* pseq = bam_get_seq(handles.bam_record);
        seq_record.sequence.resize(handles.bam_record->core.l_qseq);
        for(int i = 0; i < handles.bam_record->core.l_qseq; ++i) {
            seq_record.sequence[i] = seq_nt16_str[bam_seqi(pseq, i)];
        }
        
        // copy read base-to-reference alignment
        seq_record.aligned_bases = get_aligned_pairs(handles.bam_record);
        m_sequence_records.push_back(seq_record);
        
        /*
        printf("sequence_record[%zu] prefix: %s alignstart: [%d %d]\n", 
            m_sequence_records.size() - 1,
            m_sequence_records.back().sequence.substr(0, 20).c_str(),
            m_sequence_records.back().aligned_bases.front().ref_pos,
            m_sequence_records.back().aligned_bases.front().read_pos);
        */
    }

    // cleanup
    sam_itr_destroy(handles.itr);
    bam_destroy1(handles.bam_record);
    sam_close(handles.bam_fh);
}

void AlignmentDB::_load_events_by_region()
{
    assert(!m_region_contig.empty());
    assert(m_region_start >= 0);
    assert(m_region_end >= 0);

    BamHandles handles = _initialize_bam_itr(m_event_bam, m_region_contig, m_region_start, m_region_end);

    int result;
    while((result = sam_itr_next(handles.bam_fh, handles.itr, handles.bam_record)) >= 0) {
        EventAlignmentRecord event_record;

        std::string full_name = bam_get_qname(handles.bam_record);
        
        // Check for the template/complement suffix
        bool is_template = true;
        size_t suffix_pos = 0;
        suffix_pos = full_name.find(".template");
        if(suffix_pos == std::string::npos) {
            suffix_pos = full_name.find(".complement");
            assert(suffix_pos != std::string::npos);
            is_template = false;
        }

        std::string read_name = full_name.substr(0, suffix_pos);
        std::string fast5_path = m_fast5_name_map.get_path(read_name);

        // Do we need to load this fast5 file?
        if(m_squiggle_read_map.find(read_name) == m_squiggle_read_map.end()) {
            SquiggleRead* sr = new SquiggleRead(read_name, fast5_path);
            // Switch the read to use an alternative kmer model
            if(!m_model_type_string.empty()) {
                sr->replace_models(m_model_type_string);
            }

            m_squiggle_read_map[read_name] = sr;
        }

        event_record.sr = m_squiggle_read_map[read_name];

        // extract the event stride tag which tells us whether the
        // event indices are increasing or decreasing
        assert(bam_aux_get(handles.bam_record, "ES") != NULL);
        int event_stride = bam_aux2i(bam_aux_get(handles.bam_record, "ES"));

        // copy event alignments
        event_record.aligned_events = get_aligned_pairs(handles.bam_record, event_stride);

        event_record.rc = bam_is_rev(handles.bam_record);
        event_record.stride = event_stride;
        event_record.strand = is_template ? T_IDX : C_IDX;
        m_event_records.push_back(event_record);
        
        if(m_calibrate_on_load) {
            std::vector<EventAlignment> event_alignment = _build_event_alignment(event_record);
            fprintf(stderr, "Rescale for %s strand: %d rc: %d\n", event_record.sr->read_name.c_str(), event_record.strand, event_record.rc);
            event_record.sr->print_scaling_parameters(stderr, event_record.strand);
            fprintf(stderr, "recal events: %zu\n", event_alignment.size());
            recalibrate_model(*event_record.sr, event_record.strand, event_alignment, &gDNAAlphabet, true, false);
            event_record.sr->print_scaling_parameters(stderr, event_record.strand);
        }

        /*
        printf("event_record[%zu] name: %s stride: %d align bounds [%d %d] [%d %d]\n", 
            m_event_records.size() - 1,
            bam_get_qname(handles.bam_record),
            event_stride,
            m_event_records.back().aligned_events.front().ref_pos,
            m_event_records.back().aligned_events.front().read_pos,
            m_event_records.back().aligned_events.back().ref_pos,
            m_event_records.back().aligned_events.back().read_pos);
        */
    }

    // cleanup
    sam_itr_destroy(handles.itr);
    bam_destroy1(handles.bam_record);
    sam_close(handles.bam_fh);
}

std::vector<EventAlignment> AlignmentDB::_build_event_alignment(const EventAlignmentRecord& event_record) const
{
    std::vector<EventAlignment> alignment;
    const SquiggleRead* sr = event_record.sr;
    const Alphabet* alphabet = sr->pore_model[event_record.strand].pmalphabet;
    size_t k = sr->pore_model[event_record.strand].k;

    for(const auto& ap : event_record.aligned_events) { 

        EventAlignment ea;
        ea.ref_position = ap.ref_pos;
        if(ea.ref_position < m_region_start || ea.ref_position >= m_region_end - k) {
            continue;
        }

        ea.event_idx = ap.read_pos;

        std::string kmer = get_reference_substring(m_region_contig, ea.ref_position, ea.ref_position + k - 1);
        assert(kmer.size() == k);

        // ref data
        ea.ref_name = "read"; // not needed
        ea.read_idx = -1; // not needed
        ea.ref_kmer = kmer;
        ea.strand_idx = event_record.strand;
        ea.rc = event_record.rc;
        ea.model_kmer = kmer;
        ea.hmm_state = 'M';
        alignment.push_back(ea);
    }

    return alignment;
}

bool AlignmentDB::_find_iter_by_ref_bounds(const std::vector<AlignedPair>& pairs,
                                      int ref_start,
                                      int ref_stop,
                                      AlignedPairConstIter& start_iter,
                                      AlignedPairConstIter& stop_iter) const
{
    AlignedPairRefLBComp lb_comp;
    start_iter = std::lower_bound(pairs.begin(), pairs.end(),
                                  ref_start, lb_comp);

    stop_iter = std::lower_bound(pairs.begin(), pairs.end(),
                                 ref_stop, lb_comp);
    
    if(start_iter == pairs.end() || stop_iter == pairs.end())
        return false;
    
    // require at least one aligned reference base at or outside the boundary
    bool left_bounded = start_iter->ref_pos <= ref_start ||
                        (start_iter != pairs.begin() && (start_iter - 1)->ref_pos <= ref_start);
    
    bool right_bounded = stop_iter->ref_pos >= ref_stop ||
                        (stop_iter != pairs.end() && (stop_iter + 1)->ref_pos >= ref_start);

    return left_bounded && right_bounded;
}


bool AlignmentDB::_find_by_ref_bounds(const std::vector<AlignedPair>& pairs,
                                      int ref_start,
                                      int ref_stop,
                                      int& read_start,
                                      int& read_stop) const
{
    AlignedPairConstIter start_iter;
    AlignedPairConstIter stop_iter;
    bool bounded = _find_iter_by_ref_bounds(pairs, ref_start, ref_stop, start_iter, stop_iter);
    if(bounded) {
        read_start = start_iter->read_pos;
        read_stop = stop_iter->read_pos;
        return true;
    } else {
        return false;
    }
}
