/*
 * MinGW wrapper: makes gcc behave like MinGW.
 *
 * Copyright 2000 Manuel Novoa III
 * Copyright 2000 Francois Gouget
 * Copyright 2002 Dimitrie O. Paun
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 *
 * DESCRIPTION
 *
 * all options for gcc start with '-' and are for the most part
 * single options (no parameters as separate argument). 
 * There are of course exceptions to this rule, so here is an 
 * exhaustive list of options that do take parameters (potentially)
 * as a separate argument:
 *
 * Compiler:
 * -x language
 * -o filename
 * -aux-info filename
 *
 * Preprocessor:
 * -D name 
 * -U name
 * -I dir
 * -MF file
 * -MT target
 * -MQ target
 * (all -i.* arg)
 * -include file 
 * -imacros file
 * -idirafter dir
 * -iwithprefix dir
 * -iwithprefixbefore dir
 * -isystem dir
 * -A predicate=answer
 *
 * Linking:
 * -l library
 * -Xlinker option
 * -u symbol
 *
 * Misc:
 * -b machine
 * -V version
 * -G num  (see NOTES below)
 *
 * NOTES
 * There is -G option for compatibility with System V that
 * takes no parameters. This makes "-G num" parsing ambiguous.
 * This option is synonymous to -shared, and as such we will
 * not support it for now.
 *
 * Special interest options 
 *
 *      Assembler Option
 *          -Wa,option
 *
 *      Linker Options
 *          object-file-name  -llibrary -nostartfiles  -nodefaultlibs
 *          -nostdlib -s  -static  -static-libgcc  -shared  -shared-libgcc
 *          -symbolic -Wl,option  -Xlinker option -u symbol --image-base
 *
 *      Directory Options
 *          -Bprefix  -Idir  -I-  -Ldir  -specs=file
 *
 *      Target Options
 *          -b machine  -V version
 *
 * Please note that the Target Options are relevant to everything:
 *   compiler, linker, assembler, preprocessor.
 * 
 */ 

#include "config.h"
#include "wine/port.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>

#include "utils.h"

static const char* app_loader_template =
    "#!/bin/sh\n"
    "\n"
    "appname=\"%s\"\n"
    "# determine the application directory\n"
    "appdir=''\n"
    "case \"$0\" in\n"
    "  */*)\n"
    "    # $0 contains a path, use it\n"
    "    appdir=`dirname \"$0\"`\n"
    "    ;;\n"
    "  *)\n"
    "    # no directory in $0, search in PATH\n"
    "    saved_ifs=$IFS\n"
    "    IFS=:\n"
    "    for d in $PATH\n"
    "    do\n"
    "      IFS=$saved_ifs\n"
    "      if [ -x \"$d/$appname\" ]; then appdir=\"$d\"; break; fi\n"
    "    done\n"
    "    ;;\n"
    "esac\n"
    "\n"
    "# figure out the full app path\n"
    "if [ -n \"$appdir\" ]; then\n"
    "    apppath=\"$appdir/$appname\"\n"
    "    WINEDLLPATH=\"$appdir:$WINEDLLPATH\"\n"
    "    export WINEDLLPATH\n"
    "else\n"
    "    apppath=\"$appname\"\n"
    "fi\n"
    "\n"
    "# determine the WINELOADER\n"
    "if [ ! -x \"$WINELOADER\" ]; then WINELOADER=\"wine\"; fi\n"
    "\n"
    "# and try to start the app\n"
    "exec \"$WINELOADER\" \"$apppath\" \"$@\"\n"
;

static int keep_generated = 0;
static strarray* tmp_files;
#ifdef HAVE_SIGSET_T
static sigset_t signal_mask;
#endif

enum processor { proc_cc, proc_cxx, proc_cpp, proc_as };

enum target_cpu
{
    CPU_x86, CPU_x86_64, CPU_SPARC, CPU_ALPHA, CPU_POWERPC
};

enum target_platform
{
    PLATFORM_UNSPECIFIED, PLATFORM_APPLE, PLATFORM_SOLARIS, PLATFORM_WINDOWS
};

static const struct
{
    const char *name;
    enum target_cpu cpu;
} cpu_names[] =
{
    { "i386",    CPU_x86 },
    { "i486",    CPU_x86 },
    { "i586",    CPU_x86 },
    { "i686",    CPU_x86 },
    { "i786",    CPU_x86 },
    { "x86_64",  CPU_x86_64 },
    { "sparc",   CPU_SPARC },
    { "alpha",   CPU_ALPHA },
    { "powerpc", CPU_POWERPC }
};

static const struct
{
    const char *name;
    enum target_platform platform;
} platform_names[] =
{
    { "macos",   PLATFORM_APPLE },
    { "darwin",  PLATFORM_APPLE },
    { "solaris", PLATFORM_SOLARIS },
    { "mingw32", PLATFORM_WINDOWS },
    { "windows", PLATFORM_WINDOWS },
    { "winnt",   PLATFORM_WINDOWS }
};

struct options
{
    enum processor processor;
    enum target_cpu target_cpu;
    enum target_platform target_platform;
    const char *target;
    int shared;
    int use_msvcrt;
    int nostdinc;
    int nostdlib;
    int nostartfiles;
    int nodefaultlibs;
    int noshortwchar;
    int gui_app;
    int unicode_app;
    int compile_only;
    int force_pointer_size;
    const char* wine_objdir;
    const char* output_name;
    const char* image_base;
    const char* section_align;
    strarray* prefix;
    strarray* lib_dirs;
    strarray* linker_args;
    strarray* compiler_args;
    strarray* winebuild_args;
    strarray* files;
};

#ifdef __i386__
static const enum target_cpu build_cpu = CPU_x86;
#elif defined(__x86_64__)
static const enum target_cpu build_cpu = CPU_x86_64;
#elif defined(__sparc__)
static const enum target_cpu build_cpu = CPU_SPARC;
#elif defined(__ALPHA__)
static const enum target_cpu build_cpu = CPU_ALPHA;
#elif defined(__powerpc__)
static const enum target_cpu build_cpu = CPU_POWERPC;
#else
#error Unsupported CPU
#endif

#ifdef __APPLE__
static enum target_platform build_platform = PLATFORM_APPLE;
#elif defined(__sun)
static enum target_platform build_platform = PLATFORM_SOLARIS;
#elif defined(_WINDOWS)
static enum target_platform build_platform = PLATFORM_WINDOWS;
#else
static enum target_platform build_platform = PLATFORM_UNSPECIFIED;
#endif

static void clean_temp_files(void)
{
    unsigned int i;

    if (keep_generated) return;

    for (i = 0; i < tmp_files->size; i++)
	unlink(tmp_files->base[i]);
}

/* clean things up when aborting on a signal */
static void exit_on_signal( int sig )
{
    exit(1);  /* this will call the atexit functions */
}

static char* get_temp_file(const char* prefix, const char* suffix)
{
    int fd;
    char* tmp = strmake("%s-XXXXXX%s", prefix, suffix);

#ifdef HAVE_SIGPROCMASK
    sigset_t old_set;
    /* block signals while manipulating the temp files list */
    sigprocmask( SIG_BLOCK, &signal_mask, &old_set );
#endif
    fd = mkstemps( tmp, strlen(suffix) );
    if (fd == -1)
    {
        /* could not create it in current directory, try in /tmp */
        free(tmp);
        tmp = strmake("/tmp/%s-XXXXXX%s", prefix, suffix);
        fd = mkstemps( tmp, strlen(suffix) );
        if (fd == -1) error( "could not create temp file\n" );
    }
    close( fd );
    strarray_add(tmp_files, tmp);
#ifdef HAVE_SIGPROCMASK
    sigprocmask( SIG_SETMASK, &old_set, NULL );
#endif
    return tmp;
}

static const strarray* get_translator(struct options *opts)
{
    const char *str = NULL;
    strarray *ret;

    switch(opts->processor)
    {
    case proc_cpp:
        if (opts->target) str = strmake( "%s-cpp", opts->target );
        else str = CPP;
        break;
    case proc_cc:
    case proc_as:
        if (opts->target) str = strmake( "%s-gcc", opts->target );
        else str = CC;
        break;
    case proc_cxx:
        if (opts->target) str = strmake( "%s-g++", opts->target );
        else str = CXX;
        break;
    default:
        assert(0);
    }
    ret = strarray_fromstring( str, " " );
    if (opts->force_pointer_size)
        strarray_add( ret, strmake("-m%u", 8 * opts->force_pointer_size ));
    return ret;
}

static void compile(struct options* opts, const char* lang)
{
    strarray* comp_args = strarray_alloc();
    unsigned int j;
    int gcc_defs = 0;

    strarray_addall(comp_args, get_translator(opts));
    switch(opts->processor)
    {
	case proc_cpp: gcc_defs = 1; break;
	case proc_as:  gcc_defs = 0; break;
	/* Note: if the C compiler is gcc we assume the C++ compiler is too */
	/* mixing different C and C++ compilers isn't supported in configure anyway */
	case proc_cc:
	case proc_cxx:
            gcc_defs = strendswith(comp_args->base[0], "gcc") || strendswith(comp_args->base[0], "g++");
            break;
    }

    if (opts->target_platform == PLATFORM_WINDOWS) goto no_compat_defines;

    if (opts->processor != proc_cpp)
    {
	if (gcc_defs && !opts->wine_objdir && !opts->noshortwchar)
	{
            strarray_add(comp_args, "-fshort-wchar");
            strarray_add(comp_args, "-DWINE_UNICODE_NATIVE");
	}
        strarray_addall(comp_args, strarray_fromstring(DLLFLAGS, " "));
    }

    if (opts->target_cpu == CPU_x86_64)
    {
        strarray_add(comp_args, "-DWIN64");
        strarray_add(comp_args, "-D_WIN64");
        strarray_add(comp_args, "-D__WIN64");
        strarray_add(comp_args, "-D__WIN64__");
    }

    strarray_add(comp_args, "-DWIN32");
    strarray_add(comp_args, "-D_WIN32");
    strarray_add(comp_args, "-D__WIN32");
    strarray_add(comp_args, "-D__WIN32__");
    strarray_add(comp_args, "-D__WINNT");
    strarray_add(comp_args, "-D__WINNT__");

    if (gcc_defs)
    {
        if (opts->target_cpu == CPU_x86_64)
        {
            strarray_add(comp_args, "-D__stdcall=__attribute__((ms_abi))");
            strarray_add(comp_args, "-D__cdecl=__attribute__((ms_abi))");
            strarray_add(comp_args, "-D_stdcall=__attribute__((ms_abi))");
            strarray_add(comp_args, "-D_cdecl=__attribute__((ms_abi))");
            strarray_add(comp_args, "-D__fastcall=__attribute__((ms_abi))");
            strarray_add(comp_args, "-D_fastcall=__attribute__((ms_abi))");
        }
        else if (opts->target_platform == PLATFORM_APPLE)
        {
            /* Mac OS X uses a 16-byte aligned stack and not a 4-byte one */
            strarray_add(comp_args, "-D__stdcall=__attribute__((__stdcall__)) __attribute__((__force_align_arg_pointer__))");
            strarray_add(comp_args, "-D__cdecl=__attribute__((__cdecl__)) __attribute__((__force_align_arg_pointer__))");
            strarray_add(comp_args, "-D_stdcall=__attribute__((__stdcall__)) __attribute__((__force_align_arg_pointer__))");
            strarray_add(comp_args, "-D_cdecl=__attribute__((__cdecl__)) __attribute__((__force_align_arg_pointer__))");
        }
        else
        {
            strarray_add(comp_args, "-D__stdcall=__attribute__((__stdcall__))");
            strarray_add(comp_args, "-D__cdecl=__attribute__((__cdecl__))");
            strarray_add(comp_args, "-D_stdcall=__attribute__((__stdcall__))");
            strarray_add(comp_args, "-D_cdecl=__attribute__((__cdecl__))");
        }

	strarray_add(comp_args, "-D__fastcall=__attribute__((__fastcall__))");
	strarray_add(comp_args, "-D_fastcall=__attribute__((__fastcall__))");
	strarray_add(comp_args, "-D__declspec(x)=__declspec_##x");
	strarray_add(comp_args, "-D__declspec_align(x)=__attribute__((aligned(x)))");
	strarray_add(comp_args, "-D__declspec_allocate(x)=__attribute__((section(x)))");
	strarray_add(comp_args, "-D__declspec_deprecated=__attribute__((deprecated))");
	strarray_add(comp_args, "-D__declspec_dllimport=__attribute__((dllimport))");
	strarray_add(comp_args, "-D__declspec_dllexport=__attribute__((dllexport))");
	strarray_add(comp_args, "-D__declspec_naked=__attribute__((naked))");
	strarray_add(comp_args, "-D__declspec_noinline=__attribute__((noinline))");
	strarray_add(comp_args, "-D__declspec_noreturn=__attribute__((noreturn))");
	strarray_add(comp_args, "-D__declspec_nothrow=__attribute__((nothrow))");
	strarray_add(comp_args, "-D__declspec_novtable=__attribute__(())"); /* ignore it */
	strarray_add(comp_args, "-D__declspec_selectany=__attribute__((weak))");
	strarray_add(comp_args, "-D__declspec_thread=__thread");
    }

    strarray_add(comp_args, "-D__int8=char");
    strarray_add(comp_args, "-D__int16=short");
    strarray_add(comp_args, "-D__int32=int");
    if (opts->target_cpu == CPU_x86_64)
        strarray_add(comp_args, "-D__int64=long");
    else
        strarray_add(comp_args, "-D__int64=long long");

no_compat_defines:
    strarray_add(comp_args, "-D__WINE__");

    /* options we handle explicitly */
    if (opts->compile_only)
	strarray_add(comp_args, "-c");
    if (opts->output_name)
    {
	strarray_add(comp_args, "-o");
	strarray_add(comp_args, opts->output_name);
    }

    /* the rest of the pass-through parameters */
    for ( j = 0 ; j < opts->compiler_args->size ; j++ ) 
        strarray_add(comp_args, opts->compiler_args->base[j]);

    /* the language option, if any */
    if (lang && strcmp(lang, "-xnone"))
	strarray_add(comp_args, lang);

    /* last, but not least, the files */
    for ( j = 0; j < opts->files->size; j++ )
    {
	if (opts->files->base[j][0] != '-')
	    strarray_add(comp_args, opts->files->base[j]);
    }

    /* standard includes come last in the include search path */
    if (!opts->wine_objdir && !opts->nostdinc)
    {
        if (opts->use_msvcrt)
        {
            if (gcc_defs) strarray_add(comp_args, "-isystem" INCLUDEDIR "/msvcrt");
            else strarray_add(comp_args, "-I" INCLUDEDIR "/msvcrt");
            strarray_add(comp_args, "-D__MSVCRT__");
        }
        strarray_add(comp_args, gcc_defs ? "-isystem" INCLUDEDIR "/windows" : "-I" INCLUDEDIR "/windows" );
    }
    else if (opts->wine_objdir)
        strarray_add(comp_args, strmake("-I%s/include", opts->wine_objdir) );

    spawn(opts->prefix, comp_args, 0);
    strarray_free(comp_args);
}

static const char* compile_to_object(struct options* opts, const char* file, const char* lang)
{
    struct options copts;
    char* base_name;

    /* make a copy so we don't change any of the initial stuff */
    /* a shallow copy is exactly what we want in this case */
    base_name = get_basename(file);
    copts = *opts;
    copts.output_name = get_temp_file(base_name, ".o");
    copts.compile_only = 1;
    copts.files = strarray_alloc();
    strarray_add(copts.files, file);
    compile(&copts, lang);
    strarray_free(copts.files);
    free(base_name);

    return copts.output_name;
}

/* check if there is a static lib associated to a given dll */
static char *find_static_lib( const char *dll )
{
    char *lib = strmake("%s.a", dll);
    if (get_file_type(lib) == file_arh) return lib;
    free( lib );
    return NULL;
}

/* add specified library to the list of files */
static void add_library( strarray *lib_dirs, strarray *files, const char *library )
{
    char *static_lib, *fullname = 0;

    switch(get_lib_type(lib_dirs, library, &fullname))
    {
    case file_arh:
        strarray_add(files, strmake("-a%s", fullname));
        break;
    case file_dll:
        strarray_add(files, strmake("-d%s", fullname));
        if ((static_lib = find_static_lib(fullname)))
        {
            strarray_add(files, strmake("-a%s",static_lib));
            free(static_lib);
        }
        break;
    case file_so:
    default:
        /* keep it anyway, the linker may know what to do with it */
        strarray_add(files, strmake("-l%s", library));
        break;
    }
    free(fullname);
}

static void build(struct options* opts)
{
    static const char *stdlibpath[] = { DLLDIR, LIBDIR, "/usr/lib", "/usr/local/lib", "/lib" };
    strarray *lib_dirs, *files;
    strarray *spec_args, *link_args;
    char *output_file;
    const char *spec_o_name;
    const char *output_name, *spec_file, *lang;
    const char* winebuild = getenv("WINEBUILD");
    int generate_app_loader = 1;
    unsigned int j;

    /* NOTE: for the files array we'll use the following convention:
     *    -axxx:  xxx is an archive (.a)
     *    -dxxx:  xxx is a DLL (.def)
     *    -lxxx:  xxx is an unsorted library
     *    -oxxx:  xxx is an object (.o)
     *    -rxxx:  xxx is a resource (.res)
     *    -sxxx:  xxx is a shared lib (.so)
     *    -xlll:  lll is the language (c, c++, etc.)
     */

    if (!winebuild) winebuild = "winebuild";

    output_file = strdup( opts->output_name ? opts->output_name : "a.out" );

    /* 'winegcc -o app xxx.exe.so' only creates the load script */
    if (opts->files->size == 1 && strendswith(opts->files->base[0], ".exe.so"))
    {
	create_file(output_file, 0755, app_loader_template, opts->files->base[0]);
	return;
    }

    /* generate app loader only for .exe */
    if (opts->shared || strendswith(output_file, ".so"))
	generate_app_loader = 0;

    /* normalize the filename a bit: strip .so, ensure it has proper ext */
    if (strendswith(output_file, ".so")) 
	output_file[strlen(output_file) - 3] = 0;
    if ((output_name = strrchr(output_file, '/'))) output_name++;
    else output_name = output_file;
    if (!strchr(output_name, '.'))
        output_file = strmake("%s.%s", output_file, opts->shared ? "dll" : "exe");

    /* get the filename from the path */
    if ((output_name = strrchr(output_file, '/'))) output_name++;
    else output_name = output_file;

    /* prepare the linking path */
    if (!opts->wine_objdir)
    {
        lib_dirs = strarray_dup(opts->lib_dirs);
	for ( j = 0; j < sizeof(stdlibpath)/sizeof(stdlibpath[0]); j++ )
	    strarray_add(lib_dirs, stdlibpath[j]);
    }
    else
    {
        lib_dirs = strarray_alloc();
        strarray_add(lib_dirs, strmake("%s/dlls", opts->wine_objdir));
        strarray_add(lib_dirs, strmake("%s/libs/wine", opts->wine_objdir));
        strarray_addall(lib_dirs, opts->lib_dirs);
    }

    /* mark the files with their appropriate type */
    spec_file = lang = 0;
    files = strarray_alloc();
    for ( j = 0; j < opts->files->size; j++ )
    {
	const char* file = opts->files->base[j];
	if (file[0] != '-')
	{
	    switch(get_file_type(file))
	    {
		case file_def:
		case file_spec:
		    if (spec_file)
			error("Only one spec file can be specified\n");
		    spec_file = file;
		    break;
		case file_rc:
		    /* FIXME: invoke wrc to build it */
		    error("Can't compile .rc file at the moment: %s\n", file);
	            break;
	    	case file_res:
		    strarray_add(files, strmake("-r%s", file));
		    break;
		case file_obj:
		    strarray_add(files, strmake("-o%s", file));
		    break;
		case file_arh:
		    strarray_add(files, strmake("-a%s", file));
		    break;
		case file_so:
		    strarray_add(files, strmake("-s%s", file));
		    break;
	    	case file_na:
		    error("File does not exist: %s\n", file);
		    break;
	        default:
		    file = compile_to_object(opts, file, lang);
		    strarray_add(files, strmake("-o%s", file));
		    break;
	    }
	}
	else if (file[1] == 'l')
            add_library( lib_dirs, files, file + 2 );
	else if (file[1] == 'x')
	    lang = file;
    }
    if (opts->shared && !spec_file)
	error("A spec file is currently needed in shared mode\n");

    /* add the default libraries, if needed */
    if (!opts->nostdlib && opts->use_msvcrt) add_library(lib_dirs, files, "msvcrt");

    if (!opts->wine_objdir && !opts->nodefaultlibs) 
    {
        if (opts->gui_app) 
	{
	    add_library(lib_dirs, files, "shell32");
	    add_library(lib_dirs, files, "comdlg32");
	    add_library(lib_dirs, files, "gdi32");
	}
        add_library(lib_dirs, files, "advapi32");
        add_library(lib_dirs, files, "user32");
        add_library(lib_dirs, files, "kernel32");
    }

    if (!opts->nostartfiles) add_library(lib_dirs, files, "winecrt0");
    if (!opts->nostdlib) add_library(lib_dirs, files, "wine");

    /* run winebuild to generate the .spec.o file */
    spec_args = strarray_alloc();
    spec_o_name = get_temp_file(output_name, ".spec.o");
    strarray_add(spec_args, winebuild);
    if (verbose) strarray_add(spec_args, "-v");
    if (keep_generated) strarray_add(spec_args, "--save-temps");
    if (opts->target)
    {
        strarray_add(spec_args, "--target");
        strarray_add(spec_args, opts->target);
    }
    if (opts->force_pointer_size)
        strarray_add(spec_args, strmake("-m%u", 8 * opts->force_pointer_size ));
    strarray_addall(spec_args, strarray_fromstring(DLLFLAGS, " "));
    strarray_add(spec_args, opts->shared ? "--dll" : "--exe");
    strarray_add(spec_args, "-o");
    strarray_add(spec_args, spec_o_name);
    if (spec_file)
    {
        strarray_add(spec_args, "-E");
        strarray_add(spec_args, spec_file);
    }

    if (!opts->shared)
    {
        strarray_add(spec_args, "-F");
        strarray_add(spec_args, output_name);
        strarray_add(spec_args, "--subsystem");
        strarray_add(spec_args, opts->gui_app ? "windows" : "console");
        if (opts->unicode_app)
        {
            strarray_add(spec_args, "--entry");
            strarray_add(spec_args, "__wine_spec_exe_wentry");
        }
    }

    for ( j = 0; j < lib_dirs->size; j++ )
	strarray_add(spec_args, strmake("-L%s", lib_dirs->base[j]));

    for ( j = 0 ; j < opts->winebuild_args->size ; j++ )
        strarray_add(spec_args, opts->winebuild_args->base[j]);

    for ( j = 0; j < files->size; j++ )
    {
	const char* name = files->base[j] + 2;
	switch(files->base[j][1])
	{
	    case 'r':
		strarray_add(spec_args, files->base[j]);
		break;
	    case 'd':
	    case 'a':
	    case 'o':
		strarray_add(spec_args, name);
		break;
	}
    }

    spawn(opts->prefix, spec_args, 0);
    strarray_free (spec_args);

    /* link everything together now */
    link_args = strarray_alloc();
    strarray_addall(link_args, get_translator(opts));
    strarray_addall(link_args, strarray_fromstring(LDDLLFLAGS, " "));

    strarray_add(link_args, "-o");
    strarray_add(link_args, strmake("%s.so", output_file));

    for ( j = 0 ; j < opts->linker_args->size ; j++ ) 
        strarray_add(link_args, opts->linker_args->base[j]);

    switch (opts->target_platform)
    {
    case PLATFORM_APPLE:
        if (opts->image_base)
        {
            strarray_add(link_args, "-image_base");
            strarray_add(link_args, opts->image_base);
        }
        break;
    case PLATFORM_SOLARIS:
        {
            char *mapfile = get_temp_file( output_name, ".map" );
            const char *align = opts->section_align ? opts->section_align : "0x1000";

            create_file( mapfile, 0644, "text = A%s;\ndata = A%s;\n", align, align );
            strarray_add(link_args, strmake("-Wl,-M,%s", mapfile));
            strarray_add(tmp_files, mapfile);
        }
        break;
    default:
        break;
    }

    for ( j = 0; j < lib_dirs->size; j++ )
	strarray_add(link_args, strmake("-L%s", lib_dirs->base[j]));

    strarray_add(link_args, spec_o_name);

    for ( j = 0; j < files->size; j++ )
    {
	const char* name = files->base[j] + 2;
	switch(files->base[j][1])
	{
	    case 'l':
	    case 's':
		strarray_add(link_args, strmake("-l%s", name));
		break;
	    case 'a':
	    case 'o':
		strarray_add(link_args, name);
		break;
	}
    }

    if (!opts->nostdlib) 
    {
	strarray_add(link_args, "-lm");
	strarray_add(link_args, "-lc");
    }

    spawn(opts->prefix, link_args, 0);
    strarray_free (link_args);

    /* set the base address */
    if (opts->image_base)
    {
        const char *prelink = PRELINK;
        if (prelink[0] && strcmp(prelink,"false"))
        {
            strarray *prelink_args = strarray_alloc();
            strarray_add(prelink_args, prelink);
            strarray_add(prelink_args, "--reloc-only");
            strarray_add(prelink_args, opts->image_base);
            strarray_add(prelink_args, strmake("%s.so", output_file));
            spawn(opts->prefix, prelink_args, 1);
            strarray_free(prelink_args);
        }
    }

    /* create the loader script */
    if (generate_app_loader)
        create_file(output_file, 0755, app_loader_template, strmake("%s.so", output_name));
}


static void forward(int argc, char **argv, struct options* opts)
{
    strarray* args = strarray_alloc();
    int j;

    strarray_addall(args, get_translator(opts));

    for( j = 1; j < argc; j++ ) 
	strarray_add(args, argv[j]);

    spawn(opts->prefix, args, 0);
    strarray_free (args);
}

/*
 *      Linker Options
 *          object-file-name  -llibrary -nostartfiles  -nodefaultlibs
 *          -nostdlib -s  -static  -static-libgcc  -shared  -shared-libgcc
 *          -symbolic -Wl,option  -Xlinker option -u symbol
 *	    -framework name
 */
static int is_linker_arg(const char* arg)
{
    static const char* link_switches[] = 
    {
	"-nostartfiles", "-nodefaultlibs", "-nostdlib", "-s", 
	"-static", "-static-libgcc", "-shared", "-shared-libgcc", "-symbolic",
	"-framework"
    };
    unsigned int j;

    switch (arg[1]) 
    {
	case 'R':
	case 'z':
	case 'l':
	case 'u':
	    return 1;
        case 'W':
            if (strncmp("-Wl,", arg, 4) == 0) return 1;
	    break;
	case 'X':
	    if (strcmp("-Xlinker", arg) == 0) return 1;
	    break;
	case 'a':
	    if (strcmp("-arch", arg) == 0) return 1;
	    break;
    }

    for (j = 0; j < sizeof(link_switches)/sizeof(link_switches[0]); j++)
	if (strcmp(link_switches[j], arg) == 0) return 1;

    return 0;
}

/*
 *      Target Options
 *          -b machine  -V version
 */
static int is_target_arg(const char* arg)
{
    return arg[1] == 'b' || arg[2] == 'V';
}


/*
 *      Directory Options
 *          -Bprefix  -Idir  -I-  -Ldir  -specs=file
 */
static int is_directory_arg(const char* arg)
{
    return arg[1] == 'B' || arg[1] == 'L' || arg[1] == 'I' || strncmp("-specs=", arg, 7) == 0;
}

/*
 *      MinGW Options
 *	    -mno-cygwin -mwindows -mconsole -mthreads -municode
 */ 
static int is_mingw_arg(const char* arg)
{
    static const char* mingw_switches[] = 
    {
        "-mno-cygwin", "-mwindows", "-mconsole", "-mthreads", "-municode"
    };
    unsigned int j;

    for (j = 0; j < sizeof(mingw_switches)/sizeof(mingw_switches[0]); j++)
	if (strcmp(mingw_switches[j], arg) == 0) return 1;

    return 0;
}

static void parse_target_option( struct options *opts, const char *target )
{
    char *p, *platform, *spec = xstrdup( target );
    unsigned int i;

    /* target specification is in the form CPU-MANUFACTURER-OS or CPU-MANUFACTURER-KERNEL-OS */

    /* get the CPU part */

    if (!(p = strchr( spec, '-' ))) error( "Invalid target specification '%s'\n", target );
    *p++ = 0;
    for (i = 0; i < sizeof(cpu_names)/sizeof(cpu_names[0]); i++)
    {
        if (!strcmp( cpu_names[i].name, spec ))
        {
            opts->target_cpu = cpu_names[i].cpu;
            break;
        }
    }
    if (i == sizeof(cpu_names)/sizeof(cpu_names[0]))
        error( "Unrecognized CPU '%s'\n", spec );
    platform = p;
    if ((p = strrchr( p, '-' ))) platform = p + 1;

    /* get the OS part */

    opts->target_platform = PLATFORM_UNSPECIFIED;  /* default value */
    for (i = 0; i < sizeof(platform_names)/sizeof(platform_names[0]); i++)
    {
        if (!strncmp( platform_names[i].name, platform, strlen(platform_names[i].name) ))
        {
            opts->target_platform = platform_names[i].platform;
            break;
        }
    }

    free( spec );
    opts->target = xstrdup( target );
}

int main(int argc, char **argv)
{
    int i, c, next_is_arg = 0, linking = 1;
    int raw_compiler_arg, raw_linker_arg;
    const char* option_arg;
    struct options opts;
    char* lang = 0;
    char* str;

#ifdef SIGHUP
    signal( SIGHUP, exit_on_signal );
#endif
    signal( SIGTERM, exit_on_signal );
    signal( SIGINT, exit_on_signal );
#ifdef HAVE_SIGADDSET
    sigemptyset( &signal_mask );
    sigaddset( &signal_mask, SIGHUP );
    sigaddset( &signal_mask, SIGTERM );
    sigaddset( &signal_mask, SIGINT );
#endif

    /* setup tmp file removal at exit */
    tmp_files = strarray_alloc();
    atexit(clean_temp_files);
    
    /* initialize options */
    memset(&opts, 0, sizeof(opts));
    opts.target_cpu = build_cpu;
    opts.target_platform = build_platform;
    opts.lib_dirs = strarray_alloc();
    opts.files = strarray_alloc();
    opts.linker_args = strarray_alloc();
    opts.compiler_args = strarray_alloc();
    opts.winebuild_args = strarray_alloc();

    /* determine the processor type */
    if (strendswith(argv[0], "winecpp")) opts.processor = proc_cpp;
    else if (strendswith(argv[0], "++")) opts.processor = proc_cxx;
    
    /* parse options */
    for ( i = 1 ; i < argc ; i++ ) 
    {
        if (argv[i][0] == '-')  /* option */
	{
	    /* determine if tihs switch is followed by a separate argument */
	    next_is_arg = 0;
	    option_arg = 0;
	    switch(argv[i][1])
	    {
		case 'x': case 'o': case 'D': case 'U':
		case 'I': case 'A': case 'l': case 'u':
		case 'b': case 'V': case 'G': case 'L':
		case 'B': case 'R': case 'z':
		    if (argv[i][2]) option_arg = &argv[i][2];
		    else next_is_arg = 1;
		    break;
		case 'i':
		    next_is_arg = 1;
		    break;
		case 'a':
		    if (strcmp("-aux-info", argv[i]) == 0)
			next_is_arg = 1;
		    if (strcmp("-arch", argv[i]) == 0)
			next_is_arg = 1;
		    break;
		case 'X':
		    if (strcmp("-Xlinker", argv[i]) == 0)
			next_is_arg = 1;
		    break;
		case 'M':
		    c = argv[i][2];
		    if (c == 'F' || c == 'T' || c == 'Q')
		    {
			if (argv[i][3]) option_arg = &argv[i][3];
			else next_is_arg = 1;
		    }
		    break;
		case 'f':
		    if (strcmp("-framework", argv[i]) == 0)
			next_is_arg = 1;
		    break;
	    }
	    if (next_is_arg) option_arg = argv[i+1];

	    /* determine what options go 'as is' to the linker & the compiler */
	    raw_compiler_arg = raw_linker_arg = 0;
	    if (is_linker_arg(argv[i])) 
	    {
		raw_linker_arg = 1;
	    }
	    else 
	    {
		if (is_directory_arg(argv[i]) || is_target_arg(argv[i]))
		    raw_linker_arg = 1;
		raw_compiler_arg = !is_mingw_arg(argv[i]);
	    }

	    /* these things we handle explicitly so we don't pass them 'as is' */
	    if (argv[i][1] == 'l' || argv[i][1] == 'I' || argv[i][1] == 'L')
		raw_linker_arg = 0;
	    if (argv[i][1] == 'c' || argv[i][1] == 'L')
		raw_compiler_arg = 0;
	    if (argv[i][1] == 'o' || argv[i][1] == 'b')
		raw_compiler_arg = raw_linker_arg = 0;

	    /* do a bit of semantic analysis */
            switch (argv[i][1]) 
	    {
		case 'B':
		    str = strdup(option_arg);
		    if (strendswith(str, "/tools/winebuild"))
                    {
                        char *objdir = strdup(str);
                        objdir[strlen(objdir) - sizeof("/tools/winebuild") + 1] = 0;
                        opts.wine_objdir = objdir;
                        /* don't pass it to the compiler, this generates warnings */
                        raw_compiler_arg = raw_linker_arg = 0;
                    }
		    if (strendswith(str, "/")) str[strlen(str) - 1] = 0;
                    if (!opts.prefix) opts.prefix = strarray_alloc();
                    strarray_add(opts.prefix, str);
		    break;
                case 'b':
                    parse_target_option( &opts, option_arg );
                    break;
                case 'c':        /* compile or assemble */
		    if (argv[i][2] == 0) opts.compile_only = 1;
		    /* fall through */
                case 'S':        /* generate assembler code */
                case 'E':        /* preprocess only */
                    if (argv[i][2] == 0) linking = 0;
                    break;
		case 'f':
		    if (strcmp("-fno-short-wchar", argv[i]) == 0)
                        opts.noshortwchar = 1;
		    break;
		case 'l':
		    strarray_add(opts.files, strmake("-l%s", option_arg));
		    break;
		case 'L':
		    strarray_add(opts.lib_dirs, option_arg);
		    break;
                case 'M':        /* map file generation */
                    linking = 0;
                    break;
		case 'm':
		    if (strcmp("-mno-cygwin", argv[i]) == 0)
			opts.use_msvcrt = 1;
		    else if (strcmp("-mwindows", argv[i]) == 0)
			opts.gui_app = 1;
		    else if (strcmp("-mconsole", argv[i]) == 0)
			opts.gui_app = 0;
		    else if (strcmp("-municode", argv[i]) == 0)
			opts.unicode_app = 1;
		    else if (strcmp("-m32", argv[i]) == 0)
                    {
                        opts.force_pointer_size = 4;
			raw_linker_arg = 1;
                    }
		    else if (strcmp("-m64", argv[i]) == 0)
                    {
                        opts.force_pointer_size = 8;
			raw_linker_arg = 1;
                    }
		    break;
                case 'n':
                    if (strcmp("-nostdinc", argv[i]) == 0)
                        opts.nostdinc = 1;
                    else if (strcmp("-nodefaultlibs", argv[i]) == 0)
                        opts.nodefaultlibs = 1;
                    else if (strcmp("-nostdlib", argv[i]) == 0)
                        opts.nostdlib = 1;
                    else if (strcmp("-nostartfiles", argv[i]) == 0)
                        opts.nostartfiles = 1;
                    break;
		case 'o':
		    opts.output_name = option_arg;
		    break;
                case 's':
                    if (strcmp("-static", argv[i]) == 0) 
			linking = -1;
		    else if(strcmp("-save-temps", argv[i]) == 0)
			keep_generated = 1;
		    else if(strcmp("-shared", argv[i]) == 0)
		    {
			opts.shared = 1;
                        raw_compiler_arg = raw_linker_arg = 0;
		    }
                    break;
                case 'v':
                    if (argv[i][2] == 0) verbose++;
                    break;
                case 'W':
                    if (strncmp("-Wl,", argv[i], 4) == 0)
		    {
                        unsigned int j;
                        strarray* Wl = strarray_fromstring(argv[i] + 4, ",");
                        for (j = 0; j < Wl->size; j++)
                        {
                            if (!strcmp(Wl->base[j], "--image-base") && j < Wl->size - 1)
                            {
                                opts.image_base = strdup( Wl->base[++j] );
                                continue;
                            }
                            if (!strcmp(Wl->base[j], "--section-alignment") && j < Wl->size - 1)
                            {
                                opts.section_align = strdup( Wl->base[++j] );
                                continue;
                            }
                            if (!strcmp(Wl->base[j], "-static")) linking = -1;
                            strarray_add(opts.linker_args, strmake("-Wl,%s",Wl->base[j]));
                        }
                        strarray_free(Wl);
                        raw_compiler_arg = raw_linker_arg = 0;
                    }
		    else if (strncmp("-Wb,", argv[i], 4) == 0)
		    {
			strarray* Wb = strarray_fromstring(argv[i] + 4, ",");
			strarray_addall(opts.winebuild_args, Wb);
			strarray_free(Wb);
                        /* don't pass it to the compiler, it generates errors */
                        raw_compiler_arg = raw_linker_arg = 0;
		    }
                    break;
		case 'x':
		    lang = strmake("-x%s", option_arg);
		    strarray_add(opts.files, lang);
		    /* we'll pass these flags ourselves, explicitly */
                    raw_compiler_arg = raw_linker_arg = 0;
		    break;
                case '-':
                    if (strcmp("-static", argv[i]+1) == 0)
                        linking = -1;
                    else if (!strncmp("--sysroot", argv[i], 9) && opts.wine_objdir)
                    {
                        if (argv[i][9] == '=') opts.wine_objdir = argv[i] + 10;
                        else opts.wine_objdir = argv[++i];
                        raw_compiler_arg = raw_linker_arg = 0;
                    }
                    break;
            }

	    /* put the arg into the appropriate bucket */
	    if (raw_linker_arg) 
	    {
		strarray_add(opts.linker_args, argv[i]);
		if (next_is_arg && (i + 1 < argc)) 
		    strarray_add(opts.linker_args, argv[i + 1]);
	    }
	    if (raw_compiler_arg)
	    {
		strarray_add(opts.compiler_args, argv[i]);
		if (next_is_arg && (i + 1 < argc))
		    strarray_add(opts.compiler_args, argv[i + 1]);
	    }

	    /* skip the next token if it's an argument */
	    if (next_is_arg) i++;
        }
	else
	{
	    strarray_add(opts.files, argv[i]);
	} 
    }

    if (opts.processor == proc_cpp) linking = 0;
    if (linking == -1) error("Static linking is not supported\n");

    if (opts.files->size == 0) forward(argc, argv, &opts);
    else if (linking) build(&opts);
    else compile(&opts, lang);

    return 0;
}
