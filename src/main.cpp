#include <fstream>
#include <iostream>
#include <string>
#include <sys/types.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include <paw/parser.hpp>

#include <boost/log/trivial.hpp>
#include <boost/log/utility/setup/console.hpp>
#include <boost/log/utility/setup/common_attributes.hpp>
#include <boost/log/utility/setup/formatter_parser.hpp>

#include <graphtyper/constants.hpp>
#include <graphtyper/graph/haplotype_extractor.hpp>
#include <graphtyper/graph/constructor.hpp>
#include <graphtyper/graph/graph_serialization.hpp>
#include <graphtyper/index/indexer.hpp>
#include <graphtyper/typer/caller.hpp>
#include <graphtyper/typer/variant_map.hpp>
#include <graphtyper/typer/vcf.hpp>
#include <graphtyper/typer/vcf_operations.hpp>
#include <graphtyper/utilities/bamshrink.hpp>
#include <graphtyper/utilities/genotype.hpp>
#include <graphtyper/utilities/genotype_camou.hpp>
#include <graphtyper/utilities/genotype_sv.hpp>
#include <graphtyper/utilities/io.hpp> // gyper::get_contig_to_lengths
#include <graphtyper/utilities/options.hpp>
#include <graphtyper/utilities/system.hpp>


namespace
{

void
add_region(std::unordered_map<std::string, long> const & contig2length,
           std::vector<gyper::GenomicRegion> & regions,
           std::string const & region_str,
           long const REGION_SIZE)
{
  gyper::GenomicRegion region(region_str);
  auto find_it = contig2length.find(region.chr);

  if (find_it == contig2length.end())
  {
    BOOST_LOG_TRIVIAL(warning) << "Unable to find contig: " << region.chr << " in reference.";
    return;
  }

  // Make sure the region is inside the contig
  region.begin = std::min(region.begin, static_cast<uint32_t>(find_it->second - 1));
  region.end = std::min(region.end, static_cast<uint32_t>(find_it->second));

  // Split region if it is too large (more than 10% of given REGION_SIZE)
  while (static_cast<long>(region.end - region.begin) > (REGION_SIZE + REGION_SIZE / 10l))
  {
    gyper::GenomicRegion new_region(region);
    new_region.end = new_region.begin + REGION_SIZE;
    region.begin = new_region.end;
    regions.push_back(std::move(new_region));
  }

  regions.push_back(std::move(region));
}


std::vector<gyper::GenomicRegion>
get_regions(std::string const & ref_fn,
            std::string const & opts_region,
            std::string const & opts_region_fn,
            long const REGION_SIZE)
{
  std::unordered_map<std::string, long> contig2length = gyper::get_contig_to_lengths(ref_fn + ".fai");

  // region arg
  std::vector<gyper::GenomicRegion> regions;

  if (opts_region.size() > 0)
    add_region(contig2length, regions, opts_region, REGION_SIZE);

  if (opts_region_fn.size() > 0)
  {
    std::string line;
    std::ifstream file_in;
    file_in.open(opts_region_fn);

    while (std::getline(file_in, line))
      add_region(contig2length, regions, line, REGION_SIZE);
  }

  if (regions.size() == 0)
  {
    BOOST_LOG_TRIVIAL(error) << "No regions specified. Either use --region or --region_file option to specify regions.";
    std::exit(1);
  }

  return regions;
}


std::vector<std::string>
get_sams(std::string const & opts_sam, std::string const & opts_sams_file)
{
  std::vector<std::string> sams;

  if (opts_sam.size() > 0)
    sams.push_back(opts_sam);

  if (opts_sams_file.size() > 0)
  {
    std::ifstream file_in(opts_sams_file);

    if (!file_in.is_open())
    {
      BOOST_LOG_TRIVIAL(error) << "Could not open sams file '" << opts_sams_file << "'";
      std::exit(1);
    }

    std::string line;

    while (std::getline(file_in, line))
      sams.push_back(line);
  }

  if (sams.size() == 0)
  {
    BOOST_LOG_TRIVIAL(error) << "No SAM/BAM/CRAM files where given as input.";
    std::exit(1);
  }

  return sams;
}


std::vector<double>
get_avg_cov_by_readlen(std::string const & avg_cov_by_readlen_fn, long const expected_num)
{
  std::vector<double> avg_cov_by_readlen;

  if (avg_cov_by_readlen_fn.size() == 0)
  {
    avg_cov_by_readlen.resize(expected_num, -1.0);
  }
  else
  {
    std::string line;
    std::ifstream ifs(avg_cov_by_readlen_fn);

    if (!ifs.is_open())
    {
      BOOST_LOG_TRIVIAL(error) << "Could not open avgCovByReadlen file " << avg_cov_by_readlen_fn;
      std::exit(1);
    }

    while (std::getline(ifs, line))
    {
      double const cov_by_readlen = std::stod(line);
      avg_cov_by_readlen.push_back(cov_by_readlen);
    }

    if (static_cast<long>(avg_cov_by_readlen.size()) != expected_num)
    {
      BOOST_LOG_TRIVIAL(error) << "avg_cov_by_readlen file should have the same number of lines as there are SAMs. "
                               << avg_cov_by_readlen.size() << " != " << expected_num << "\n";
      std::exit(1);
    }
  }

  return avg_cov_by_readlen;
}


void
setup_logger()
{
  gyper::Options & opts = *(gyper::Options::instance());

  if (opts.vverbose)
  {
    boost::log::core::get()->set_filter
    (
      boost::log::trivial::severity >= boost::log::trivial::debug
    );
  }
  else if (opts.verbose)
  {
    boost::log::core::get()->set_filter
    (
      boost::log::trivial::severity >= boost::log::trivial::info
    );
  }
  else
  {
    boost::log::core::get()->set_filter
    (
      boost::log::trivial::severity >= boost::log::trivial::warning
    );
  }

  boost::log::add_common_attributes();
  boost::log::register_simple_formatter_factory<boost::log::trivial::severity_level, char>("Severity");
  std::string log_format = "[%TimeStamp%] <%Severity%> %Message%";

  if (opts.log.size() == 0 || opts.log == "-")
  {
    // Create a console sink log
    boost::log::add_console_log(std::clog,
                                boost::log::keywords::auto_flush = true,
                                boost::log::keywords::format = log_format
                                );
  }
  else
  {
    // Create a file sink
    opts.sink = boost::log::add_file_log
                (
      boost::log::keywords::file_name = opts.log,
      boost::log::keywords::auto_flush = true,
      boost::log::keywords::format = log_format
                );
  }
}


int
subcmd_bamshrink(paw::Parser & parser)
{
  bamshrink::Options opts;

  parser.parse_positional_argument(opts.bamPathIn, "bamPathIn", "Input BAM file path.");
  parser.parse_option(opts.avgCovByReadLen,
                      'a',
                      "avg-cov-by-readlen",
                      "Average coverage divided by read length.",
                      "D"
                      );

  parser.parse_option(opts.bamIndex, ' ', "index", "Input BAM bai/CRAM crai index file.");
  parser.parse_option(opts.bamPathOut, 'o', "output", "Output BAM file.");
  parser.parse_option(opts.interval,
                      'i',
                      "interval",
                      "Interval/region to filter on in format chrA:N-M, where chrA is the contig name, "
                      "N is the begin position, and M is the end position of the interval."
                      );

  parser.parse_option(opts.intervalFile, 'I', "interval-file",
                      "File with interval(s)/region(s) to filter on."
                      );

  parser.parse_option(opts.maxFragLen, 'f', "max-fragment-length", "Maximum fragment length allowed.", "N");
  parser.parse_option(opts.minNumMatching, 'm', "min-num-matching",
                      "Minumum number of matching bases in read.", "N"
                      );

  parser.finalize();
  setup_logger();
  return bamshrink::main(opts);
}


int
subcmd_call(paw::Parser & parser)
{
  bool skip_writing_calls_vcf{false};
  bool no_new_variants{false};
  bool is_writing_hap{true};

  gyper::Options & opts = *(gyper::Options::instance());

  long minimum_variant_support = 5;
  double minimum_variant_support_ratio = 0.25;

  std::string graph_fn;
  std::string index_dir = "<graph>_gti";
  std::string output_dir;
  std::string sam;
  std::string sams;

  parser.parse_option(index_dir, ' ', "index", "Path to index directory.");
  parser.parse_option(minimum_variant_support, ' ',
                      "minimum_variant_support", "Minimum variant support for it to be considered.");
  parser.parse_option(minimum_variant_support_ratio, ' ',
                      "minimum_variant_support_ratio", "Minimum variant support ratio for it to be considered.");
  parser.parse_option(opts.no_asterisks, ' ', "no_asterisks", "Set to avoid using asterisk in VCF output.");
  parser.parse_option(opts.no_decompose, ' ', "no_decompose", "Set to avoid decomposing variants in VCF output.");
  parser.parse_option(no_new_variants, ' ', "no_new_variants", "Set if no new variants should be discovered.");
  parser.parse_option(opts.no_variant_overlapping, ' ', "no_variant_overlapping", "Set to avoid that variants "
                                                                                  "overlap each other in the VCF output");
  parser.parse_option(opts.is_one_genotype_per_haplotype, ' ', "one_genotype_per_haplotype",
                      "Do not consider multiple variants together.");
  parser.parse_option(output_dir, 'O', "output", "Output directory.");
  parser.parse_option(sam, 's', "sam", "SAM/BAM/CRAM to analyze.");
  parser.parse_option(sams, 'S', "sams", "File with SAM/BAM/CRAMs to analyze (one per line).");
  parser.parse_option(skip_writing_calls_vcf, ' ', "skip_writing_calls_vcf", "Do not write VCF with calls.");
#ifndef NDEBUG
  parser.parse_option(opts.stats, ' ', "stats", "Directory for statistics files.");
#endif // NDEBUG
  parser.parse_option(opts.threads, 't', "threads", "Max. number of threads to use.");
  parser.parse_option(opts.variant_suffix_id, ' ', "suffix_id", "Suffix which will be added behind each variant.");

  parser.parse_positional_argument(graph_fn, "GRAPH", "Path to graph.");

  parser.finalize();
  setup_logger();

  std::vector<std::string> sams_fn = get_sams(sam, sams);

  if (index_dir == "<graph>_gti")
    index_dir = graph_fn + "_gti";

#ifndef NDEBUG
  if (opts.stats.size() > 0 && !gyper::is_directory(opts.stats))
    mkdir(opts.stats.c_str(), 0755);
#endif // NDEBUG

  gyper::call(sams_fn,
              graph_fn,
              index_dir,
              output_dir,
              minimum_variant_support,
              minimum_variant_support_ratio,
              !skip_writing_calls_vcf,
              !no_new_variants,
              is_writing_hap);

  return 0;
}


int
subcmd_check(paw::Parser & parser)
{
  std::string graph_fn;
  parser.parse_positional_argument(graph_fn, "GRAPH", "Path to graph.");
  parser.finalize();
  setup_logger();

  gyper::load_graph(graph_fn);
  gyper::graph.check();
  gyper::graph.print();

  return 0;
}


int
subcmd_construct(paw::Parser & parser)
{
  gyper::Options & opts = *(gyper::Options::instance());

  std::string graph_fn;
  bool is_skip_indexing{false};
  bool is_sv_graph{false};
  bool use_tabix{false};
  std::string ref_fn;
  std::string region;
  std::string vcf_fn;

  // Parse options
  parser.parse_option(is_skip_indexing, ' ', "skip_indexing", "Set to skip indexing the graph.");
  parser.parse_option(is_sv_graph, ' ', "sv_graph", "Set to construct an SV graph.");
  parser.parse_option(opts.add_all_variants, ' ', "output_all_variants", "Set to create a graph with every possible "
                                                                         "haplotype on overlapping variants.");
  parser.parse_option(use_tabix, ' ', "use_tabix", "Set to use tabix index to extract variants of the given region.");
  parser.parse_option(vcf_fn, ' ', "vcf", "VCF variant input.");

  parser.parse_positional_argument(graph_fn, "GRAPH", "Path to graph.");
  parser.parse_positional_argument(ref_fn, "REF.FA", "Reference genome in FASTA format.");
  parser.parse_positional_argument(region, "REGION", "Genomic region to construct graph for.");

  parser.finalize();
  setup_logger();

  region.erase(std::remove(region.begin(), region.end(), ','), region.end());
  BOOST_LOG_TRIVIAL(info) << "Constructing a graph for region " << region;

  gyper::check_file_exists(ref_fn);
  gyper::check_file_exists_or_empty(vcf_fn);

  gyper::construct_graph(ref_fn, vcf_fn, region, is_sv_graph, true, use_tabix);

  if (!is_skip_indexing)
    gyper::index_graph(graph_fn + "_gti");

  gyper::save_graph(graph_fn);
  BOOST_LOG_TRIVIAL(info) << "Graph saved at " << graph_fn;
  return 0;
}


int
subcmd_discover(paw::Parser & parser)
{
  gyper::Options & opts = *(gyper::Options::instance());

  std::string graph_fn;
  std::string region;
  std::string sam;
  std::string sams;
  std::string output_dir = "results";

  long minimum_variant_support = 5;
  double minimum_variant_support_ratio = 0.25;

  parser.parse_option(minimum_variant_support, ' ',
                      "minimum_variant_support", "Minimum variant support for it to be considered.");
  parser.parse_option(minimum_variant_support_ratio, ' ',
                      "minimum_variant_support_ratio", "Minimum variant support ratio for it to be considered.");
  parser.parse_option(opts.max_files_open, ' ', "max_files_open", "Max. number of files open at the same time.");
  parser.parse_option(output_dir, 'O', "output", "Output directory.");
  parser.parse_option(sam, 's', "sam", "SAM/BAM/CRAM to analyze.");
  parser.parse_option(sams, 'S', "sams", "File with SAM/BAM/CRAMs to analyze (one per line).");
  parser.parse_option(opts.threads, 't', "threads", "Max. number of threads to use.");

  parser.parse_positional_argument(graph_fn, "GRAPH", "Path to graph.");
  parser.parse_positional_argument(region, "REGION", "Genomic region to discover on.");

  parser.finalize();
  setup_logger();

#ifndef NDEBUG
  gyper::check_file_exists(graph_fn);
#endif // NDEBUG

  region.erase(std::remove(region.begin(), region.end(), ','), region.end());
  BOOST_LOG_TRIVIAL(info) << "Starting to discover in region " << region;

  // Parse SAM files
  std::vector<std::string> sams_fn = get_sams(sam, sams);

  if (!gyper::is_directory(output_dir))
    mkdir(output_dir.c_str(), 0755);

  gyper::discover_directly_from_bam(graph_fn,
                                    sams_fn,
                                    region,
                                    output_dir,
                                    minimum_variant_support,
                                    minimum_variant_support_ratio);
  return 0;
}


int
subcmd_discovery_vcf(paw::Parser & parser)
{
  std::string graph_fn;
  std::string output_fn = "-";
  std::string variant_maps_fn;

  parser.parse_option(output_fn, 'o', "output", "Output VCF file name.");

  parser.parse_positional_argument(graph_fn, "GRAPH", "Path to graph.");
  parser.parse_positional_argument(variant_maps_fn, "VARIANT_MAPS", "Path to a file with list of variant maps.");
  parser.finalize();
  setup_logger();

#ifndef NDEBUG
  gyper::check_file_exists(graph_fn);
#endif // NDEBUG

  gyper::load_graph(graph_fn);
  gyper::graph.generate_reference_genome();
  gyper::VariantMap varmap;
  varmap.load_many_variant_maps(variant_maps_fn);
  varmap.filter_varmap_for_all();
  gyper::Vcf discovery_vcf;
  varmap.get_vcf(discovery_vcf, output_fn);
  discovery_vcf.write();
  return 0;
}


int
subcmd_genotype(paw::Parser & parser)
{
  gyper::Options & opts = *(gyper::Options::instance());

  std::string avg_cov_by_readlen_fn;
  std::string output_dir = "results";
  std::string opts_region = "";
  std::string opts_region_file = "";
  std::string ref_fn;
  std::string sam;
  std::string sams;
  bool force_copy_reference{false};
  bool force_no_copy_reference{false};
  bool no_filter_on_proper_pairs{false};

  // Parse options
  parser.parse_option(avg_cov_by_readlen_fn,
                      ' ',
                      "avg_cov_by_readlen",
                      "File with average boverage by read length.");

  parser.parse_option(opts.max_files_open,
                      ' ',
                      "max_files_open",
                      "Select how many files can be open at the same time.");

  parser.parse_option(opts.no_asterisks, ' ', "no_asterisks", "Set to avoid using asterisk in VCF output.");
  parser.parse_option(opts.no_bamshrink, ' ', "no_bamshrink",
                      "Set to skip bamShrink.");
  parser.parse_option(opts.no_cleanup, ' ', "no_cleanup",
                      "Set to skip removing temporary files. Useful for debugging.");
  parser.parse_option(opts.no_decompose, ' ', "no_decompose", "Set to avoid decomposing variants in VCF output.");
  parser.parse_option(opts.no_variant_overlapping, ' ', "no_variant_overlapping", "Set to avoid that variants "
                                                                                  "overlap each other in the VCF output");
  parser.parse_option(no_filter_on_proper_pairs, ' ', "no_filter_on_proper_pairs",
                      "Set to disable filter on proper pairs. This should be set if you have unpaired reads.");
  parser.parse_option(force_copy_reference, ' ', "force_copy_reference",
                      "Force copy of the reference FASTA to temporary folder.");
  parser.parse_option(force_no_copy_reference, ' ', "force_no_copy_reference",
                      "Force that the reference FASTA is NOT copied to temporary folder.");
  parser.parse_option(output_dir, 'O', "output", "Output directory.");
  parser.parse_option(opts_region, 'r', "region", "Genomic region to genotype.");
  parser.parse_option(opts_region_file, 'R', "region_file", "File with genomic regions to genotype.");
  parser.parse_option(opts.threads, 't', "threads", "Max. number of threads to use.");
  parser.parse_option(sam, 's', "sam", "SAM/BAM/CRAM to analyze.");
  parser.parse_option(sams, 'S', "sams", "File with SAM/BAM/CRAMs to analyze (one per line).");

#ifndef NDEBUG
  parser.parse_option(opts.stats, ' ', "stats", "Directory for statistics files.");
#endif // NDEBUG

  parser.parse_option(opts.vcf, ' ', "vcf", "Input VCF file with variant sites.");

  parser.parse_positional_argument(ref_fn, "REF.FA", "Reference genome in FASTA format.");
  parser.finalize();
  setup_logger();

  opts.filter_on_proper_pairs = !no_filter_on_proper_pairs;
  BOOST_LOG_TRIVIAL(info) << "Running the 'genotype' subcommand.";

#ifndef NDEBUG
  // Create stats directory if it doesn't exist
  if (opts.stats.size() > 0 && !gyper::is_directory(opts.stats))
    mkdir(opts.stats.c_str(), 0755);
#endif // NDEBUG

  // Get the genomic regions to process from the --region and --region_file options
  std::vector<gyper::GenomicRegion> regions = get_regions(ref_fn, opts_region, opts_region_file, 50000);

  // Get the SAM/BAM/CRAM file names
  std::vector<std::string> sams_fn = get_sams(sam, sams);

  // If neither force copy reference or force no copy reference we determine it from number of SAMs
  bool is_copy_reference = force_copy_reference || (!force_no_copy_reference && sams_fn.size() >= 100);

  // Get the avgCovByReadLen for each of the SAM/BAM/CRAM
  std::vector<double> avg_cov_by_readlen = get_avg_cov_by_readlen(avg_cov_by_readlen_fn, sams_fn.size());

  gyper::genotype_regions(ref_fn,
                          sams_fn,
                          regions,
                          output_dir,
                          avg_cov_by_readlen,
                          is_copy_reference);
  return 0;
}


int
subcmd_genotype_sv(paw::Parser & parser)
{
  gyper::Options & opts = *(gyper::Options::instance());

  std::string avg_cov_by_readlen_fn;
  std::string output_dir = "sv_results";
  std::string opts_region = "";
  std::string opts_region_file = "";
  std::string ref_fn;
  std::string sam;
  std::string sams;
  std::string sv_vcf;
  bool force_copy_reference = false;
  bool force_no_copy_reference = false;

  // Parse options
  parser.parse_option(opts.max_files_open,
                      ' ',
                      "max_files_open",
                      "Select how many files can be open at the same time.");
  parser.parse_option(opts.no_cleanup, ' ', "no_cleanup",
                      "Set to skip removing temporary files. Useful for debugging.");
  parser.parse_option(force_copy_reference, ' ', "force_copy_reference",
                      "Force copy of the reference FASTA to temporary folder.");
  parser.parse_option(force_no_copy_reference, ' ', "force_no_copy_reference",
                      "Force that the reference FASTA is NOT copied to temporary folder.");
  parser.parse_option(output_dir, 'O', "output", "Output directory.");
  parser.parse_option(opts_region, 'r', "region", "Genomic region to genotype.");
  parser.parse_option(opts_region_file, 'R', "region_file", "File with genomic regions to genotype.");
  parser.parse_option(opts.threads, 't', "threads", "Max. number of threads to use.");
  parser.parse_option(sam, 's', "sam", "SAM/BAM/CRAM to analyze.");
  parser.parse_option(sams, 'S', "sams", "File with SAM/BAM/CRAMs to analyze (one per line).");
#ifndef NDEBUG
  parser.parse_option(opts.stats, ' ', "stats", "Directory for statistics files.");
#endif // NDEBUG
  //parser.parse_option(opts.vcf, ' ', "vcf", "Input VCF file with SNP/indel variant sites.");

  parser.parse_positional_argument(ref_fn, "REF.FA", "Reference genome in FASTA format.");
  parser.parse_positional_argument(sv_vcf, "vcf",
                                   "Input VCF file with structural variant sites and optionally also SNP/indel sites.");
  parser.finalize();
  setup_logger();

  BOOST_LOG_TRIVIAL(info) << "Running the 'genotype_sv' subcommand.";

#ifndef NDEBUG
  // Create stats directory if it doesn't exist
  if (opts.stats.size() > 0 && !gyper::is_directory(opts.stats))
    mkdir(opts.stats.c_str(), 0755);
#endif // NDEBUG

  // Get the genomic regions to process from the --region and --region_file options
  std::vector<gyper::GenomicRegion> regions = get_regions(ref_fn, opts_region, opts_region_file, 1000000);

  // Get the SAM/BAM/CRAM file names
  std::vector<std::string> sams_fn = get_sams(sam, sams);

  // If neither force copy reference or force no copy reference we determine it from number of SAMs
  bool is_copy_reference = force_copy_reference || (!force_no_copy_reference && sams_fn.size() >= 100);

  gyper::genotype_sv_regions(ref_fn,
                             sv_vcf,
                             sams_fn,
                             regions,
                             output_dir,
                             is_copy_reference);
  return 0;
}


int
subcmd_genotype_camou(paw::Parser & parser)
{
  gyper::Options & opts = *(gyper::Options::instance());

  std::string avg_cov_by_readlen_fn;
  std::string interval_fn;
  std::string output_dir = "results";
  std::string ref_fn;
  std::string region;
  std::string sam;
  std::string sams;

  parser.parse_option(avg_cov_by_readlen_fn,
                      ' ',
                      "avg_cov_by_readlen",
                      "File with average coverage by read length.");

  parser.parse_option(opts.max_files_open, ' ', "max_files_open",
                      "Select how many files can be open at the same time.");

  parser.parse_option(opts.no_bamshrink, ' ', "no_bamshrink",
                      "Set to skip bamShrink.");

  parser.parse_option(opts.no_cleanup, ' ', "no_cleanup",
                      "Set to skip removing temporary files. Useful for debugging.");

  parser.parse_option(region,
                      'r',
                      "region",
                      "Region to genotype. If not specified, then the first interval in the \"interval-file\" is used.");

  parser.parse_option(sam, 's', "sam", "SAM/BAM/CRAM to analyze.");
  parser.parse_option(sams, 'S', "sams", "File with SAM/BAM/CRAMs to analyze (one per line).");
  parser.parse_option(opts.threads, 't', "threads", "Max. number of threads to use.");

  parser.parse_positional_argument(ref_fn, "REF.FA", "Reference genome in FASTA format.");
  parser.parse_positional_argument(interval_fn, "interval-file",
                                   "3-column BED type file with interval(s)/region(s) to filter on.");

  parser.finalize();
  setup_logger();

  // Do not filter on MAPQ in camou calling
  opts.filter_on_mapq = false;

  // Get the SAM/BAM/CRAM file names
  std::vector<std::string> sams_fn = get_sams(sam, sams);

  // Get the avgCovByReadLen for each of the SAM/BAM/CRAM
  std::vector<double> avg_cov_by_readlen = get_avg_cov_by_readlen(avg_cov_by_readlen_fn, sams_fn.size());

  gyper::genotype_camou(interval_fn,
                        ref_fn,
                        sams_fn,
                        region,
                        output_dir,
                        avg_cov_by_readlen);

  return 0;
}


int
subcmd_haplotypes(paw::Parser & parser)
{
  gyper::Options & opts = *(gyper::Options::instance());

  std::string graph_fn;
  std::string haplotypes_fn;
  std::string output_fn = "-";
  std::string region;

  parser.parse_option(opts.max_extracted_haplotypes, ' ', "max_extracted_haplotypes",
                      "Max. number of extracted haplotypes per site.");
  parser.parse_option(output_fn, 'o', "output", "Output VCF file name.");
  parser.parse_option(region, 'r', "region", "Region to print variant in.");

  parser.parse_positional_argument(graph_fn, "GRAPH", "Path to graph.");
  parser.parse_positional_argument(haplotypes_fn, "HAPS", "Path to file with haplotype files (one per line).");
  parser.finalize();
  setup_logger();

  gyper::extract_to_vcf(graph_fn,
                        haplotypes_fn,
                        output_fn,
                        region,
                        false); // is_splitting_vars
  return 0;
}


int
subcmd_vcf_break_down(paw::Parser & parser)
{
  std::string graph_fn;
  std::string output_fn = "-";
  std::string region;
  std::string vcf_fn;

  parser.parse_option(output_fn, 'o', "output", "Output VCF file name.");
  parser.parse_option(region, 'r', "region", "Region to print variant in.");

  parser.parse_positional_argument(graph_fn, "GRAPH", "Path to graph.");
  parser.parse_positional_argument(vcf_fn, "VCF", "Path to VCF file to break down.");

  parser.finalize();
  setup_logger();

  // load the graph
  gyper::load_graph(graph_fn);
  gyper::vcf_break_down(vcf_fn, output_fn, region);
  return 0;
}


int
subcmd_vcf_concatenate(paw::Parser & parser)
{
  std::vector<std::string> vcfs;
  bool no_sort{false};
  std::string output_fn = "-";
  bool sites_only{false};
  std::string region;

  parser.parse_option(no_sort, ' ', "no_sort", "Set to skip sorting the variants.");
  parser.parse_option(output_fn, 'o', "output", "Output VCF file name.");
  parser.parse_option(sites_only, ' ', "sites_only", "Set to write only variant site information.");
  parser.parse_option(region, 'r', "region", "Region to print variant in.");

  parser.parse_remaining_positional_arguments(vcfs, "vcfs...", "VCFs to concatenate");

  parser.finalize();
  setup_logger();

  gyper::vcf_concatenate(vcfs, output_fn, no_sort, sites_only, region);
  return 0;
}


int
subcmd_vcf_merge(paw::Parser & parser)
{
  std::vector<std::string> vcfs;
  std::string output_fn;
  std::string file_list;

  parser.parse_option(output_fn, 'o', "output", "Output VCF file name.");
  parser.parse_option(file_list, ' ', "file_list", "File containing VCFs to merge.");

  parser.parse_remaining_positional_arguments(vcfs, "vcfs...", "VCFs to merge");

  parser.finalize();
  setup_logger();

  if (file_list.size() > 0)
  {
    std::ifstream files;
    files.open(file_list);

    if (!files.is_open())
    {
      BOOST_LOG_TRIVIAL(error) << "Could not open file '" << file_list << "'\n";
      return 1;
    }

    for (std::string line; std::getline(files, line);)
      vcfs.push_back(line);
  }

  gyper::vcf_merge(vcfs, output_fn);
  return 0;
}


} // namespace anon


int
main(int argc, char ** argv)
{
  int ret{0};
  paw::Parser parser(argc, argv);
  parser.set_name("GraphTyper");
  parser.set_version(graphtyper_VERSION_MAJOR, graphtyper_VERSION_MINOR, graphtyper_VERSION_PATCH);

  try
  {
    std::string subcmd{};

    parser.add_subcommand("bamshrink", "Run bamShrink.");
    parser.add_subcommand("call", "Call variants of a graph.");
    parser.add_subcommand("check", "Check a GraphTyper graph (useful for debugging).");
    parser.add_subcommand("construct", "Construct a graph.");
    parser.add_subcommand("discover", "Discover variants from SAM/BAM/CRAMs.");
    parser.add_subcommand("discovery_vcf", "Create a VCF with discovered variants.");
    parser.add_subcommand("genotype", "Run the SNP/indel genotyping pipeline.");
    parser.add_subcommand("genotype_camou", "(WIP) Run the camou SNP/indel genotyping pipeline.");
    parser.add_subcommand("genotype_sv", "Run the structural variant (SV) genotyping pipeline.");
    parser.add_subcommand("haplotypes", "Create a VCF from genotyped haplotypes.");
    parser.add_subcommand("index", "(deprecated) Index a graph.");
    parser.add_subcommand("vcf_break_down", "Break down/decompose a VCF file.");
    parser.add_subcommand("vcf_concatenate", "Concatenate VCF files.");
    parser.add_subcommand("vcf_merge", "Merge VCF files.");
    parser.parse_subcommand(subcmd);

    gyper::Options & opts = *(gyper::Options::instance());

    parser.parse_option(opts.log, 'l', "log", "Set path to log file.");
    parser.parse_option(opts.verbose, 'v', "verbose", "Set to output verbose logging.");
    parser.parse_option(opts.vverbose, ' ', "vverbose", "Set to output very verbose logging.");

    if (subcmd == "bamshrink")
      ret = subcmd_bamshrink(parser);
    else if (subcmd == "call")
      ret = subcmd_call(parser);
    else if (subcmd == "check")
      ret = subcmd_check(parser);
    else if (subcmd == "construct")
      ret = subcmd_construct(parser);
    else if (subcmd == "discover")
      ret = subcmd_discover(parser);
    else if (subcmd == "discovery_vcf")
      ret = subcmd_discovery_vcf(parser);
    else if (subcmd == "genotype")
      ret = subcmd_genotype(parser);
    else if (subcmd == "genotype_camou")
      ret = subcmd_genotype_camou(parser);
    else if (subcmd == "genotype_sv")
      ret = subcmd_genotype_sv(parser);
    else if (subcmd == "haplotypes")
      ret = subcmd_haplotypes(parser);
    else if (subcmd == "index")
    {
      std::cerr << "GraphTyper's 'index' subcommand deprecated. "
                << "Graph is now indexed automatically after construction.\n";
      std::exit(1);
    }
    else if (subcmd == "vcf_break_down")
      ret = subcmd_vcf_break_down(parser);
    else if (subcmd == "vcf_concatenate")
      ret = subcmd_vcf_concatenate(parser);
    else if (subcmd == "vcf_merge")
      ret = subcmd_vcf_merge(parser);
    else if (subcmd.size() == 0)
    {
      parser.finalize();
    }
    else
    {
      parser.finalize();
      std::exit(1);
    }
  }
  catch (paw::exception::help const & e)
  {
    std::cout << e.what();
    return 0;
  }
  catch (std::exception const & e)
  {
    std::cerr << e.what();
    return 1;
  }

  if (gyper::Options::const_instance()->sink)
    gyper::Options::const_instance()->sink->flush();

  return ret;
}
