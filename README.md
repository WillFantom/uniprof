uniprof: Xen Domain Stack Profiler
========================

uniprof is a stack tracer/profiler for Xen domains, primarily, but not
exclusively, for unikernels. It allows producing stack traces of a running
domU, at specified sampling rates, from outside (typically dom0).
These traces can then be aggregated to produce profiling information and
visualization, for example, via [flame
graphs](https://github.com/brendangregg/FlameGraph).

Building and using the stack tracer
----------------------------------

### Quick start
If you already have Xen and its tools installed (and why wouldn't you if you
want to use this tool), then you can simply do the following:

    ./configure
    make

Afterwards, run ./uniprof --help for an overview of the command line.

### Supported CPU architectures
uniprof supports both x86 and ARM.

### Build options
The configure script has several options that you can set to influence the
build.

###### --with-libxc / --with-libxencall
These two options control which library uniprof is built against. There are
two options: either libxc, the classic, but less performant approach, and
libxencall/libxenforeignmemory, introduced with Xen 4.7, that are faster,
lower-level libraries to issue hypercalls and map memory between domains. By
default, libxencall/libxenforeignmemory are used if they are available. You can
enforce using libxc by giving the --with-libxc option to configure. Note that
this will not work when building on ARM, because libxc does not provide the
necessary memory address translation functionality for ARM. (This also means
that you have to use Xen 4.7 or newer on ARM.)

###### --with-xen=\<dir>
Instead of using the system headers and libraries installed by Xen, you can
also build against a Xen source tree. Make sure you at the very least ran a
"make tools" inside that source tree, so that the required libraries are
available.

###### --without-libunwind
If the configure script finds the libunwind-xen library, it will by default
add support for it to uniprof. If, for some reason, you do not want this
behavior, you can disable building against libunwind.

### Profiling a domain using the frame pointer register
As a first test, start a unikernel domain, note its domid, and run

    ./uniprof -F 1 -T 1 - [domid]

You should see an output of a stack trace. If you don't, then you probably
compiled your unikernel with -fomit-frame-pointer. Either recompile your
unikernel with -fno-omit-frame-pointer, or see below on how to use a specially
patched libunwind to unwind the stack.

However, even then, you will notice that you only get a bunch of memory
addresses as output, similar to this:

    0x422c3c
    0x413930
    0x41953c

This isn't exactly helpful. Indeed, you need a symbol table to translate
addresses into function names. To create that, locate your unikernel binary
and run `nm -n [image] > [image].syms` on it. For this, your binary must NOT
be stripped! You are, however, welcome to strip it after running `nm`, if
you prefer.

You now have two options: online resolution or offline resolution. Online
resolution means to do the symbol resolution during the stack trace. This is
helpful for rapid checking of functionality and the occasional single stack
trace. To do so, add the `-s [symbolfile]` command line option to
uniprof. You will now see more helpful output:

    block_domain+0x88
    schedule+0x230
    xenbus_thread_func+0x128

However, this incurs a slight performance overhead, since each
address must be resolved to a symbol during the stack walk. For performance
profiling, you might therefore prefer to only record the addresses and resolve
them offline after finishing the profiling run. To do so, use the
`symbolize` tool provided.

### Profiling a domain using libunwind-xen
If you cannot or do not want to use the frame pointer register to unwind the
stack, you can use a specially patched version of libunwind (available at
https://github.com/sysml/libunwind) to do the unwinding. This requires
you to have the ELF binary available, and that binary to contain an `.eh_frame`
section (which generally is there by default). uniprof then compares the
current IP register to information in the `.eh_frame` to assess the size of the
currently running function's stack frame. It then restores the return address
and iterates the lookup process until the end of the stack is reached.

Note that this is currently significantly slower than the frame pointer
method, due to overhead introduced by the libunwind core functions. However,
it allows you to create stack traces for VMs that don't use the frame
pointer.

To use this feature, use the `-e` or `-E` option when starting uniprof,
providing the ELF binary of the kernel as parameter. If the binary is
unstripped, the `-E` option will also give you symbol resolution.

### Using uniprof for standard Operating Systems
If your Xen domain is not a unikernel, but rather a standard kernel with
user-space tools running concurrently, you can still use uniprof, albeit with
some limitations. First of all, you will need to acquire a symbol table, which
you will need to resolve the stack addresses to function names. Especially in
case of dynamically-loaded parts of the kernel (e.g., Linux kernel modules),
you cannot simply rely on a statically-created symbol table via the `nm`
method. For Linux, you will instead need the dynamic symbol table, which you
can retrieve from /proc/kallsyms if the kernel was compiled with
CONFIG\_KALLSYMS. Note that this only allows you to resolve kernel symbols;
userspace symbols are still non-resolved, so you will need to filter those out
from your traces to create useful profiling. When using the Linux kernel, you
will probably have more success and more meaningful results if you set up
`perf` profiling inside your domain, but uniprof can still help traving from
the outside in case tracing from the inside is impossible (e.g., tracking down
insidious kernel bugs) or when using operating systems with less-developed
profiling tools.

Further documentation
---------------------

I presented uniprof at the Xen Developer and Design Summit 2017. Here's a
[video of the talk](https://www.youtube.com/watch?v=aUzrm8hBMzc), and
[the slide set](http://flosch.eu/papers/2017-xensummit-uniprof-slides.pdf).

You can also get a more general overview and problem statement from the
[two-page summary of uniprof](http://flosch.eu/papers/2017-sigcomm-uniprof.pdf)
written as a poster abstract for SIGCOMM 2017.
