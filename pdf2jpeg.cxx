#include <iostream>
#include <memory>
#include <sstream>
#include <system_error>
#include <filesystem>
#include <string>
#include <vector>
#include <set>
#include <system_error>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <locale>
#include <algorithm>
#include <cstdlib>
#include <boost/program_options.hpp>
#include <poppler/cpp/poppler-document.h>
#include <poppler/cpp/poppler-page.h>
#include <poppler/cpp/poppler-page-renderer.h>

using std::cout, std::cerr, std::endl;

namespace fs = std::filesystem;
namespace po = boost::program_options;
namespace pl = poppler;

namespace poppler {
    typedef std::shared_ptr<document> document_ptr;
    typedef std::shared_ptr<page> page_ptr;
}

template <class CharT, typename... Rest>
auto to_lower(const std::basic_string<CharT, Rest...>& from) {
    static auto tolower_bind = std::bind( std::tolower<CharT>, std::placeholders::_1 , std::locale() );
    std::basic_string<CharT, Rest...> to;
    std::transform(from.begin(), from.end(), std::back_inserter(to), tolower_bind);
    return to;
}

std::string counter_to_string(size_t val, size_t len) {
    std::string result(len, '0');
    for(auto it = result.rbegin(); val and it != result.rend(); *it++ = '0' + val % 10, val /= 10);
    return result;
}

void scan_folder_for_pdf(const fs::path& path, std::set<fs::path>& result, const bool follow_symlink) noexcept {
    std::error_code error;
    fs::file_status file_status = fs::symlink_status(path, error);
    if(error) {
        cerr << error.message() << endl;
        return;
    }

    if( follow_symlink and fs::is_symlink(file_status) ) {
        fs::path spath = fs::read_symlink(path, error);
        if(error)
            cerr << error.message() << endl;
        else
            scan_folder_for_pdf(spath, result, follow_symlink);
    }
    else if( fs::is_regular_file(file_status) ) {
        if( path.has_extension() and to_lower( path.extension().native() ) == ".pdf" )
            result.insert(path);
    }
    else if( fs::is_directory(file_status) ) {
        fs::directory_iterator dir_it(path, fs::directory_options::skip_permission_denied, error);
        if(error)
            cerr << error.message() << endl;
        else
            for(const auto& dir_entry: dir_it)
                scan_folder_for_pdf(dir_entry, result, follow_symlink);
    }
}

int main(int argc, char** argv)
{
    bool follow_symlink = false;
    bool verbose = false;
    std::string img_type = "jpg";
    fs::path dest_folder = ".";
    size_t img_dpi = 200;
    std::vector<std::string> input_folders;
    std::vector<std::string> img_formats_list = pl::image::supported_image_formats();
    std::string img_formats_help = "set image type (";
    for(const auto& format: img_formats_list)
        img_formats_help += format + '|';
    img_formats_help.back() = ')';

    po::options_description desc("Allowed options");
    desc.add_options()
            ("help,h", "produce this message")
            ("symlinks,s", "follow symlinks if set")
            ("verbose,v", "display progress")
            ("type,t", po::value<std::string>(&img_type)->default_value("jpg"), img_formats_help.c_str())
            ("dpi,D", po::value<size_t>(&img_dpi)->default_value(200), "set image DPI")
            ("dest,d", po::value<fs::path>(&dest_folder)->default_value("."), "destination folder")
            ("input-folders,i", po::value< std::vector<std::string> >(&input_folders), "list of input folders");

    po::positional_options_description p;
        p.add("input-folders", -1);

    po::variables_map vm;
    try {
        po::store(po::command_line_parser(argc, argv)
            .options(desc)
            .positional(p)
            .run(), vm);
    }
    catch(po::error& e) {
        cerr << e.what() << endl << desc << endl;
        return 1;
    }
    po::notify(vm);

    //Check input parameters
    if(vm.count("help")) {
        cerr << desc << endl; 
        return 1; 
    }
    if(vm.count("symlinks")) {
        follow_symlink = true;
    }
    if(vm.count("verbose")) {
        verbose = true;
    }
    if(not input_folders.size()) {
        cerr << "No input folders" << endl << desc << endl;
        return 1;
    }
    if(vm.count("dest")) {
        std::error_code error;
        fs::file_status file_status = fs::symlink_status(dest_folder, error);
        if(error or not fs::is_directory(file_status) ) {
            cerr << desc << endl;
            return 1;
        }
    }
    if(vm.count("type")) {
        img_type = to_lower(img_type);
        if( std::find(img_formats_list.begin(), img_formats_list.end(), img_type) == img_formats_list.end() ) {
            cerr << "Wrong format" << endl << desc << endl;
            return 1;
        }
    }

    //Scan given list of folders for PDF files
    std::set<fs::path> pdf_files;
    for(const auto& folder: input_folders)
        scan_folder_for_pdf(folder, pdf_files, follow_symlink);

    //Stuff for threads managment
    std::mutex threads_sync;
    std::condition_variable threads_cv;
    size_t threads_count = 0;

    //Get hardware threads to set the maximum threads to run
    size_t threads_max = std::thread::hardware_concurrency();
    if( not threads_max )
        threads_max = 4;

    //Procedure to convert and store one page
    //Will be run in working threads
    auto convert_proc = [&threads_sync, &threads_cv, &threads_count, img_type, img_dpi, verbose]
        (pl::document_ptr doc_ptr, int i, fs::path img_name) -> void
    {
        { 
            std::unique_lock<std::mutex> lock(threads_sync);
            ++threads_count;
        }

        pl::page_ptr page_ptr( doc_ptr->create_page(i) );
        if( page_ptr )
            poppler::page_renderer().render_page(page_ptr.get(), img_dpi, img_dpi).save( img_name.native(), img_type);
        
        { 
            std::unique_lock<std::mutex> lock(threads_sync);
            if( not page_ptr ) cerr << "Cannot render: " << img_name.native() << endl;
            else if( verbose ) cout << "Done: " << img_name.native() << endl;
            --threads_count;
        }
        page_ptr.reset();
        doc_ptr.reset();
        threads_cv.notify_one();
    };

    //Main loop through the pdf files found
    for(const auto& pdf: pdf_files) {
        if( verbose ) {
            std::unique_lock<std::mutex> lock(threads_sync);
            cout << "Start: " << pdf.native() << endl;
        }

        //Reading the pdf file
        pl::document_ptr doc_ptr( pl::document::load_from_file( pdf.native() ) );

        if( not doc_ptr ) {
            cerr << "Cannot open file: " << pdf.native() << endl;
            continue;
        }

        fs::path dest_dir_name = dest_folder;
        dest_dir_name /= pdf.filename();
        dest_dir_name.replace_extension("");
        
        //Create a folder to store pdf file`s pages
        fs::create_directory( dest_dir_name );
        
        for(int i = 0; i < doc_ptr->pages(); ++i) {
            fs::path img_name = dest_dir_name;
            img_name /= dest_dir_name.filename();
            img_name += fs::path( std::string(" ") + counter_to_string(i, 4) + '.' + img_type );
        
            std::unique_lock<std::mutex> lock(threads_sync);
            threads_cv.wait(lock, [&threads_count, &threads_max] { return threads_count < threads_max; } );
            
            std::thread convert_task(convert_proc, doc_ptr, i, img_name);
            //Dont want the thread`s ownership. So, dettach it.
            convert_task.detach();
        }
        doc_ptr.reset();
    }
    //Wait last threads to finish
    std::unique_lock<std::mutex> lock(threads_sync);
    threads_cv.wait(lock, [&threads_count] { return threads_count == 0; } );

    return 0;
}
