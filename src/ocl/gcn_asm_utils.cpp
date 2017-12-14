/*******************************************************************************
 *
 * MIT License
 *
 * Copyright (c) 2017 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 *******************************************************************************/
#include <cassert>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <miopen/config.h>
#include <miopen/env.hpp>
#include <miopen/errors.hpp>
#include <miopen/gcn_asm_utils.hpp>
#include <miopen/manage_ptr.hpp>
#include <miopen/write_file.hpp>
#include <miopen/kernel.hpp>
#include <sstream>

#ifdef __linux__
#include <paths.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif // __linux__

MIOPEN_DECLARE_ENV_VAR(MIOPEN_EXPERIMENTAL_GCN_ASM_PATH)

struct tmp_dir_env
{
    static const char* value() { return "TMPDIR"; }
};

#ifdef __linux__
class TempFile
{
    public:
    TempFile(const std::string& path_template)
        : _path(GetTempDirectoryPath() + "/" + path_template + "-XXXXXX")
    {
        _fd = mkstemp(&_path[0]);
        if(_fd == -1)
        {
            MIOPEN_THROW("Error: TempFile: mkstemp()");
        }
    }

    ~TempFile()
    {
        const int remove_rc = std::remove(_path.c_str());
        const int close_rc  = close(_fd);
        if(remove_rc != 0 || close_rc != 0)
        {
#ifndef NDEBUG // Be quiet in release versions.
            std::fprintf(stderr,
                         "Error: TempFile: On removal of '%s', remove_rc = %d, close_rc = %d.\n",
                         _path.c_str(),
                         remove_rc,
                         close_rc);
#endif
        }
    }

    inline const std::string& Path() { return _path; }
    inline operator const std::string&() { return _path; }

    private:
    std::string _path;
    int _fd;

    static const std::string GetTempDirectoryPath()
    {
        const auto path = miopen::GetStringEnv(tmp_dir_env{});
        if(path != nullptr)
        {
            return path;
        }
#if defined(P_tmpdir)
        return P_tmpdir; // a string literal, if defined.
#elif defined(_PATH_TMP)
        return _PATH_TMP; // a string literal, if defined.
#else
        return "/tmp";
#endif
    }
};
#endif

static std::string CleanupPath(const char* p);

// Redirecting both input and output is not supported.
static int ExecuteGcnAssembler(const std::string& p, std::istream* in, std::ostream* out);

std::string GetGcnAssemblerPathImpl()
{
    const auto asm_path_env_p = miopen::GetStringEnv(MIOPEN_EXPERIMENTAL_GCN_ASM_PATH{});
    if(asm_path_env_p)
    {
        return CleanupPath(asm_path_env_p);
    }
#ifdef MIOPEN_AMDGCN_ASSEMBLER // string literal generated by CMake
    return CleanupPath(MIOPEN_AMDGCN_ASSEMBLER);
#else
    return "";
#endif
}

std::string GetGcnAssemblerPath()
{
    static const auto result = GetGcnAssemblerPathImpl();
    return result;
}

bool ValidateGcnAssemblerImpl()
{
#ifdef __linux__
    const auto path = GetGcnAssemblerPath();
    if(path.empty())
    {
        return false;
    }
    if(!std::ifstream(path).good())
    {
        return false;
    }

    std::stringstream clang_stdout;
    std::string clang_result_line;
    auto clang_rc = ExecuteGcnAssembler(path + " --version", nullptr, &clang_stdout);

    if(clang_rc != 0)
    {
        return false;
    }

    std::getline(clang_stdout, clang_result_line);
    if(clang_result_line.find("clang") != std::string::npos)
    {
        while(!clang_stdout.eof())
        {
            std::getline(clang_stdout, clang_result_line);
            if(clang_result_line.find("Target: ") != std::string::npos)
            {
                return clang_result_line.find("amdgcn") != std::string::npos;
            }
        }
    }
#endif // __linux__
    return false;
}

bool ValidateGcnAssembler()
{
    static bool result = ValidateGcnAssemblerImpl();
    return result;
}

static int ExecuteGcnAssembler(const std::string& p, std::istream* in, std::ostream* out)
{
#ifdef __linux__
    const auto redirect_stdin  = (in != nullptr);
    const auto redirect_stdout = (out != nullptr);

    assert(!(redirect_stdin && redirect_stdout));

    const auto file_mode = redirect_stdout ? "r" : "w";
    MIOPEN_MANAGE_PTR(FILE*, pclose) pipe{popen(p.c_str(), file_mode)};

    if(!pipe)
        MIOPEN_THROW("Error: X-AMDGCN-ASM: popen()");

    if(redirect_stdin || redirect_stdout)
    {
        std::array<char, 1024> buffer{};

        if(redirect_stdout)
        {
            while(!feof(pipe.get()))
                if(fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr)
                    *out << buffer.data();
        }
        else
        {
            while(!in->eof())
            {
                in->read(buffer.data(), buffer.size() - 1);
                buffer[in->gcount()] = 0;

                if(fputs(buffer.data(), pipe.get()) == EOF)
                    MIOPEN_THROW("Error: X-AMDGCN-ASM: fputs()");
            }
        }
    }

    auto status = pclose(pipe.release());
    return WEXITSTATUS(status);
#else
    (void)p;
    (void)in;
    (void)out;
    return -1;
#endif // __linux__
}

static std::string CleanupPath(const char* p)
{
    std::string path(p);
    static const char bad[] = "!#$*;<>?@\\^`{|}";
    for(char* c = &path[0]; c < (&path[0] + path.length()); ++c)
    {
        if(std::iscntrl(*c))
        {
            *c = '_';
            continue;
        }
        for(const char* b = &bad[0]; b < (&bad[0] + sizeof(bad) - 1); ++b)
        {
            if(*b == *c)
            {
                *c = '_';
                break;
            }
        }
    }
    return path;
}

/*
 * Temporary function which emulates online assembly feature of OpenCL-on-ROCm being developed.
 * Not intended to be used in production code, so error handling is very straghtforward,
 * just catch whatever possible and throw an exception.
 */
void AmdgcnAssemble(std::string& source, const std::string& params)
{
#ifdef __linux__
    TempFile outfile("amdgcn-asm-out-XXXXXX");

    const auto args = " -x assembler -target amdgcn--amdhsa " + params + " - -o " + outfile.Path();

    std::istringstream clang_stdin(source);
    const auto clang_path = GetGcnAssemblerPath();
    const auto clang_rc   = ExecuteGcnAssembler(clang_path + " " + args, &clang_stdin, nullptr);
    if(clang_rc != 0)
        MIOPEN_THROW("Assembly error(" + std::to_string(clang_rc) + ")");

    std::ifstream file(outfile, std::ios::binary | std::ios::ate);
    bool outfile_read_failed = false;
    do
    {
        const auto size = file.tellg();
        if(size == -1)
        {
            outfile_read_failed = true;
            break;
        }
        source.resize(size, '\0');
        file.seekg(std::ios::beg);
        if(file.fail())
        {
            outfile_read_failed = true;
            break;
        }
        if(file.rdbuf()->sgetn(&source[0], size) != size)
        {
            outfile_read_failed = true;
            break;
        }
    } while(false);
    file.close();
    if(outfile_read_failed)
    {
        MIOPEN_THROW("Error: X-AMDGCN-ASM: outfile_read_failed");
    }
#else
    (void)source; // -warning
    (void)params; // -warning
    MIOPEN_THROW("Error: X-AMDGCN-ASM: online assembly under Windows is not supported");
#endif //__linux__
}

static bool GcnAssemblerHas34765Impl()
{
    auto p = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path();
    miopen::WriteFile(miopen::GetKernelSrc("bugzilla_34765_detect"), p);
    auto src = p.string();
    try 
    {
        AmdgcnAssemble(src, "-mcpu=gfx900");
        return true;
    }
    catch(...) 
    {
        return false;
    }
}

bool GcnAssemblerHas34765()
{
    const static bool b = GcnAssemblerHas34765Impl();
    return b;
}

template <>
void GenerateClangDefsym<const std::string&>(std::ostream& stream,
                                             const std::string& name,
                                             const std::string& value)
{
    stream << " -Wa,-defsym," << name << "=" << value;
}

std::string MakeLutKey(int w, int h, int c, int n, int k, int u, int v, int dir, int CUs)
{
    std::ostringstream ss;
    ss << w << ";" << h << ";" << c << ";" << n << ";" << k << ";" << u << ";" << v << ";" << dir
       << ";" << CUs;
    return ss.str();
}

std::string MakeLutKey(int w, int h, int c, int n, int k, int dir, int CUs)
{
    return MakeLutKey(w, h, c, n, k, 1, 1, dir, CUs);
}
