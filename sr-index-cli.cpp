#include "CLI11.hpp"
#include "include/sr-index/sr_index.h"
#include "include/sr-index/construct.h"
#include "include/sr-index/config.h"
#include "sri_cli_utils.h"

enum SRI_TYPE{
    SRI_INDEX=0,
    SRI_VALID_MARKS=1,
    SRI_VALID_AREA=2,
};

struct arguments{
    std::string input_file;
    std::string output_file;
    std::string tmp_dir;
    std::string pat_file;
    size_t n_threads{};
    size_t ssamp=4;
    sri::SAAlgo sa_algo = sri::SDSL_LIBDIVSUFSORT;
    SRI_TYPE index_type = SRI_VALID_AREA;
};

class MyFormatter : public CLI::Formatter {
public:
    MyFormatter() : Formatter() {}
    std::string make_option_opts(const CLI::Option *) const override { return ""; }
};

template<class index_type>
void test_count(std::string input_file, std::string& pat_file, std::string index_name){

    index_type index;
    sdsl::load_from_file(index, input_file);

    uint64_t n_pats, pat_len;
    std::vector<std::string> pat_list = file2pat_list(pat_file, n_pats, pat_len);

    size_t acc_time=0;
    std::pair<size_t, size_t> ans;
    size_t acc_count=0;
    for(auto const& p : pat_list) {
        MEASURE(index.Count(p), acc_time, ans, std::chrono::nanoseconds)
        acc_count+=ans.second-ans.first;
    }

    std::cout<<"Index type \""<<index_name<<"\""<<std::endl;
    std::cout<<"\tTotal number of occurrences "<<acc_count<<std::endl;
    std::cout<<"\t"<<double(acc_time)/double(n_pats)<<" nanosecs/pat"<<std::endl;
    std::cout<<"\t"<<double(acc_time)/double(acc_count)<<" nanosecs/occ"<<std::endl;
}

template<class index_type>
void test_locate(std::string input_file, std::string& pat_file, std::string index_name){

    index_type index;
    sdsl::load_from_file(index, input_file);

    uint64_t n_pats, pat_len;
    std::vector<std::string> pat_list = file2pat_list(pat_file, n_pats, pat_len);

    size_t acc_time=0;
    std::pair<size_t, size_t> ans;
    size_t acc_count=0;
    for(auto const& p : pat_list) {
        std::vector<size_t> occ;
        MEASURE(index.Locate(p), acc_time, occ, std::chrono::nanoseconds)
        acc_count+=occ.size();
    }

    std::cout<<"Index type \""<<index_name<<"\""<<std::endl;
    std::cout<<"\tTotal number of occurrences "<<acc_count<<std::endl;
    std::cout<<"\t"<<double(acc_time)/double(n_pats)<<" nanosecs/pat"<<std::endl;
    std::cout<<"\t"<<double(acc_time)/double(acc_count)<<" nanosecs/occ"<<std::endl;
}

static void parse_app(CLI::App& app, struct arguments& args){
    
	auto fmt = std::make_shared<MyFormatter>();

    fmt->column_width(23);
    app.formatter(fmt);

    auto * build = app.add_subcommand("build");
    build->add_option("TEXT", args.input_file, "Input text to be indexed")->check(CLI::ExistingFile)->required();
    build->add_option("-s,--ssamp", args.ssamp, "Subsampling parameter (def 4)")->default_val(4);
    build->add_option("-i,--index-type", args.index_type, "Subsample r-index variant to be constructed (0=standard, 1=valid_marks, 2=valid_area [def=2])")->default_val(SRI_VALID_AREA)->check(CLI::Range(0,2));
    build->add_option("-t,--threads", args.n_threads, "Maximum number of working threads")->default_val(1);
    build->add_option("-a,--sa-algorithm", args.sa_algo, "Algorithm for computing the SA (0=LIBDIVSUFSORT, 1=SE_SAIS, 2=BIG_BWT [def=0])")->default_val(LIBDIVSUFSORT)->check(CLI::Range(0,2));
    build->add_option("-o,--output", args.output_file, "Output file where the index will be stored");
    build->add_option("-T,--tmp", args.tmp_dir, "Temporary folder (def. /tmp/sri.xxxx)")-> check(CLI::ExistingDirectory)->default_val("/tmp");

    auto * count = app.add_subcommand("count");
    count->add_option("INDEX", args.input_file, "Index file")->check(CLI::ExistingFile)->required();
    count->add_option("PAT_FILE", args.pat_file, "List of patterns")->check(CLI::ExistingFile)->required();
    count->add_option("-i,--index-type", args.index_type, "Subsample r-index variant (0=standard, 1=valid_marks, 2=valid_area)")->required();

    auto * locate = app.add_subcommand("locate");
    locate->add_option("INDEX", args.input_file, "Index file")->check(CLI::ExistingFile)->required();
    locate->add_option("PAT_FILE", args.pat_file, "List of patterns")->check(CLI::ExistingFile)->required();
    locate->add_option("-i,--index-type", args.index_type, "Subsample r-index variant (0=standard, 1=valid_marks, 2=valid_area)")->required();

    auto * bkdown = app.add_subcommand("breakdown");
    bkdown->add_option("INDEX", args.input_file, "Index to be read")->check(CLI::ExistingFile)->required();
    bkdown->add_option("-i,--index-type", args.index_type, "Subsample r-index variant (0=standard, 1=valid_marks, 2=valid_area)")->required();

    app.require_subcommand(1,1);
}

template<class index_type>
void build_int(std::string input_text, size_t ssamp_val, std::filesystem::path tmp_path, sri::SAAlgo sa_algo, std::string& output_file){
    index_type index(ssamp_val);
    sri::Config config(input_text, tmp_path, sa_algo);
    sri::construct(index, input_text, config);
    sdsl::store_to_file(index, output_file);
}

template<class index_type>
void breakdown_int(std::string input_index){
    index_type index;
    sdsl::load_from_file(index, input_index);
    std::vector<std::pair<std::string, size_t>> parts = index.breakdown();
    std::cout<<"Index file: "<<input_index<<std::endl;
    std::cout<<"Subsampling parameter: "<<index.SubsampleRate()<<std::endl;
    size_t acc=0;
    for(auto const& part : parts){
        acc+=part.second;
    }
    for(auto const& part : parts){
        std::cout<<"\t"<<part.first<<": "<<part.second<<" bytes ("<<100*(double)part.second/(double)acc<<"%)"<<std::endl;
    }
    std::cout<<"Total: "<<acc<<" bytes"<<std::endl;
}

int main(int argc, char** argv) {

    arguments args;
    CLI::App app("Subsample r-index");
    parse_app(app, args);

    CLI11_PARSE(app, argc, argv);

    if(args.output_file.empty()) args.output_file = args.output_file = std::filesystem::path(args.input_file).filename();

    if(app.got_subcommand("build")) {
        std::cout<<"Building the subsample r-index for "<<args.input_file<<std::endl;
        std::cout<<"\tSubsampling parameter: "<<args.ssamp<<std::endl;
        switch (args.index_type) {
            case SRI_INDEX:
                args.output_file = std::filesystem::path(args.output_file).replace_extension("sri");
                build_int<sri::SrIndex<>>(args.input_file, args.ssamp, std::filesystem::current_path(), args.sa_algo, args.output_file);
                break;
            case SRI_VALID_MARKS:
                args.output_file = std::filesystem::path(args.output_file).replace_extension("sri_vm");
                build_int<sri::SrIndexValidMark<>>(args.input_file, args.ssamp, std::filesystem::current_path(), args.sa_algo, args.output_file);
                break;
            case SRI_VALID_AREA:
                args.output_file = std::filesystem::path(args.output_file).replace_extension("sri_va");
                build_int<sri::SrIndexValidArea<>>(args.input_file, args.ssamp, std::filesystem::current_path(), args.sa_algo, args.output_file);
                break;
            default:
                std::cerr<<"Unknown subsample r-index type"<<std::endl;
                exit(1);
        }
        std::cout<<"The output index was stored in "<<args.output_file<<std::endl;
    } else if(app.got_subcommand("count")){
        switch (args.index_type) {
            case SRI_INDEX:
                std::cout<<"Index type: sri"<<std::endl;
                test_count<sri::SrIndex<>>(args.input_file, args.pat_file, "sri");
                break;
            case SRI_VALID_MARKS:
                std::cout<<"Index type: sri_valid_marks"<<std::endl;
                test_count<sri::SrIndexValidMark<>>(args.input_file, args.pat_file, "sri_valid_marks");
                break;
            case SRI_VALID_AREA:
                std::cout<<"Index type: sri_valid_area"<<std::endl;
                test_count<sri::SrIndexValidArea<>>(args.input_file, args.pat_file, "sri_valid_area");
                break;
            default:
                std::cerr<<"Unknown subsample r-index type"<<std::endl;
                exit(1);
        }
    } else if(app.got_subcommand("locate")){
        switch (args.index_type) {
            case SRI_INDEX:
                std::cout<<"Index type: sri"<<std::endl;
                test_locate<sri::SrIndex<>>(args.input_file, args.pat_file, "sri");
                break;
            case SRI_VALID_MARKS:
                std::cout<<"Index type: sri_valid_marks"<<std::endl;
                test_locate<sri::SrIndexValidMark<>>(args.input_file, args.pat_file, "sri_valid_marks");
                break;
            case SRI_VALID_AREA:
                std::cout<<"Index type: sri_valid_area"<<std::endl;
                test_locate<sri::SrIndexValidArea<>>(args.input_file, args.pat_file, "sri_valid_area");
                break;
            default:
                std::cerr<<"Unknown subsample r-index type"<<std::endl;
                exit(1);
        }
    } else if(app.got_subcommand("breakdown")){
        switch (args.index_type) {
            case SRI_INDEX:
                std::cout<<"Index type: sri"<<std::endl;
                breakdown_int<sri::SrIndex<>>(args.input_file);
                break;
            case SRI_VALID_MARKS:
                std::cout<<"Index type: sri_valid_marks"<<std::endl;
                breakdown_int<sri::SrIndexValidMark<>>(args.input_file);
                break;
            case SRI_VALID_AREA:
                std::cout<<"Index type: sri_valid_area"<<std::endl;
                breakdown_int<sri::SrIndexValidArea<>>(args.input_file);
                break;
            default:
                std::cerr<<"Unknown subsample r-index type"<<std::endl;
                exit(1);
        }
    } else {
        std::cerr<<" Unknown command "<<std::endl;
        exit(1);
    }
    return 0;
}
