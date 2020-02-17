#pragma once

#include <string> // std::string
#include <vector> // std::vector
#include <unordered_map> // std::unordered_map

#include <htslib/sam.h>

#include <graphtyper/utilities/hts_record.hpp>
#include <graphtyper/utilities/hts_utils.hpp>
#include <graphtyper/utilities/hts_store.hpp>
#include <graphtyper/utilities/options.hpp>


namespace gyper
{

class HtsReader
{
public:
  htsFile * fp = nullptr; // htslib file pointer
  std::vector<std::string> samples;
  int sample_index_offset = 0;
  int rg_index_offset = 0;

private:
  int ret = 0; // return value of the most recently read record
  bam1_t * rec = nullptr; // The most recently read record
  std::vector<bam1_t *> records; // A bulk of records that have the same position.
  // TODO check if it is faster to first check if the records are sorted
  HtsStore & store;
  std::unordered_map<std::string, long> rg2index; // associates read groups with indices of that rg
  std::vector<int> rg2sample_i; // uses the rg index to determine the sample index


public:
  HtsReader(HtsStore & _store);

  void open(std::string const & path);
  void close();
  int set_reference(std::string const & reference_path);
  void set_sample_index_offset(int new_sample_index_offset);
  void set_rg_index_offset(int new_rg_index_offset);

  bam1_t * get_next_read(bam1_t * old_record);
  bam1_t * get_next_read();
  bam1_t * get_next_pos();

  void get_sample_and_rg_index(long & sample_i, long & rg_i, bam1_t * rec) const;
  long get_num_rg() const;
};

} // namespace hts
