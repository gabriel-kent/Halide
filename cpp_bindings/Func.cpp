#include <llvm-c/ExecutionEngine.h>
#include <llvm/ExecutionEngine/GenericValue.h>
#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/PassManager.h>
#include <llvm/Analysis/Passes.h>
#include <llvm/Transforms/Scalar.h>
#include <llvm/Transforms/IPO/PassManagerBuilder.h>
#include <llvm/Target/TargetData.h>
#include <llvm/Assembly/PrintModulePass.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Transforms/IPO.h>
#include <sys/time.h>

#include "../src/buffer.h"
#include "Func.h"
#include "Util.h"
#include "Var.h"
#include "Image.h"
#include "Uniform.h"
#include "elf.h"
#include <sstream>

#include <dlfcn.h>

namespace Halide {
    
    extern "C" { typedef struct CUctx_st *CUcontext; }
    CUcontext cuda_ctx = 0;
    
    bool use_gpu() {
        char* target = getenv("HL_TARGET");
        return (target != NULL && strcasecmp(target, "ptx") == 0);
    }
    
    ML_FUNC2(makeVectorizeTransform);
    ML_FUNC2(makeUnrollTransform);
    ML_FUNC4(makeBoundTransform);
    ML_FUNC5(makeSplitTransform);
    ML_FUNC3(makeTransposeTransform);
    ML_FUNC2(makeChunkTransform);
    ML_FUNC1(makeRootTransform);
    ML_FUNC2(makeParallelTransform);
    ML_FUNC2(makeRandomTransform);
    
    ML_FUNC1(doConstantFold);
    
    ML_FUNC3(makeDefinition);
    ML_FUNC6(addScatterToDefinition);
    ML_FUNC0(makeEnv);
    ML_FUNC2(addDefinitionToEnv);
    
    ML_FUNC4(makeSchedule);
    ML_FUNC3(doLower);

    ML_FUNC0(makeNoviceGuru);
    ML_FUNC1(loadGuruFromFile);
    ML_FUNC2(saveGuruToFile);

    ML_FUNC1(printStmt);
    ML_FUNC1(printSchedule);
    ML_FUNC1(makeBufferArg); // name
    ML_FUNC2(makeScalarArg); // name, type
    ML_FUNC3(doCompile); // name, args, stmt
    ML_FUNC3(doCompileToFile); // name, args, stmt
    ML_FUNC2(makePair);
    ML_FUNC3(makeTriple);

    struct FuncRef::Contents {
        Contents(const Func &f) :
            f(f) {}
        Contents(const Func &f, const Expr &a) :
            f(f), args {a} {fixArgs();}
        Contents(const Func &f, const Expr &a, const Expr &b) :
            f(f), args {a, b} {fixArgs();}
        Contents(const Func &f, const Expr &a, const Expr &b, const Expr &c) :
            f(f), args {a, b, c} {fixArgs();}
        Contents(const Func &f, const Expr &a, const Expr &b, const Expr &c, const Expr &d) :
            f(f), args {a, b, c, d} {fixArgs();}
        Contents(const Func &f, const Expr &a, const Expr &b, const Expr &c, const Expr &d, const Expr &e) :
            f(f), args {a, b, c, d, e} {fixArgs();}
        Contents(const Func &f, const std::vector<Expr> &args) : f(f), args(args) {fixArgs();}

        void fixArgs() {
            for (size_t i = 0; i < args.size(); i++) {
                if (args[i].type() != Int(32)) {
                    args[i] = cast<int>(args[i]);
                }
            }
        }

        // A pointer to the function object that this lhs defines.
        Func f;
        std::vector<Expr> args;
    };

    struct Func::Contents {
        Contents() :
            name(uniqueName('f')), functionPtr(NULL) {}
        Contents(Type returnType) : 
            name(uniqueName('f')), returnType(returnType), functionPtr(NULL) {}
      
        Contents(std::string name) : 
            name(name), functionPtr(NULL) {}
        Contents(std::string name, Type returnType) : 
            name(name), returnType(returnType), functionPtr(NULL) {}
      
        Contents(const char * name) : 
            name(name), functionPtr(NULL) {}
        Contents(const char * name, Type returnType) : 
            name(name), returnType(returnType), functionPtr(NULL) {}
        
        const std::string name;
        
        static llvm::ExecutionEngine *ee;
        static llvm::FunctionPassManager *fPassMgr;
        static llvm::PassManager *mPassMgr;
        
        // The scalar value returned by the function
        Expr rhs;
        std::vector<Expr> args;
        MLVal arglist;
        Type returnType;

        // A handle to an update function
        std::unique_ptr<Func> update;
        
        /* The ML definition object (name, return type, argnames, body)
           The body here evaluates the function over an entire range,
           and the arg list will include a min and max value for every
           free variable. */
        MLVal definition;
        
        /* A list of schedule transforms to apply when realizing. These should be
           partially applied ML functions that map a schedule to a schedule. */
        std::vector<MLVal> scheduleTransforms;        
        MLVal applyScheduleTransforms(MLVal);

        // The compiled form of this function
        mutable void (*functionPtr)(void *);
        mutable void (*copyToHost)(buffer_t *);
        mutable void (*freeBuffer)(buffer_t *);
    };

    llvm::ExecutionEngine *Func::Contents::ee = NULL;
    llvm::FunctionPassManager *Func::Contents::fPassMgr = NULL;
    llvm::PassManager *Func::Contents::mPassMgr = NULL;
    
    FuncRef::FuncRef(const Func &f) :
        contents(new FuncRef::Contents(f)) {
    }

    FuncRef::FuncRef(const Func &f, const Expr &a) : 
        contents(new FuncRef::Contents(f, a)) {
    }

    FuncRef::FuncRef(const Func &f, const Expr &a, const Expr &b) :
        contents(new FuncRef::Contents(f, a, b)) {
    }

    FuncRef::FuncRef(const Func &f, const Expr &a, const Expr &b, const Expr &c) :
        contents(new FuncRef::Contents(f, a, b, c)) {
    }

    FuncRef::FuncRef(const Func &f, const Expr &a, const Expr &b, const Expr &c, const Expr &d) : 
        contents(new FuncRef::Contents(f, a, b, c, d)) {
    }

    FuncRef::FuncRef(const Func &f, const Expr &a, const Expr &b, const Expr &c, const Expr &d, const Expr &e) : 
        contents(new FuncRef::Contents(f, a, b, c, d, e)) {
    }

    FuncRef::FuncRef(const Func &f, const std::vector<Expr> &args) :
        contents(new FuncRef::Contents(f, args)) {
    }

    FuncRef::FuncRef(const FuncRef &other) :
        contents(other.contents) {
    }

    void FuncRef::operator=(const Expr &e) {
        contents->f.define(contents->args, e);
    }

    void FuncRef::operator+=(const Expr &e) {
        std::vector<Expr> gather_args(contents->args.size());
        for (size_t i = 0; i < gather_args.size(); i++) {
            gather_args[i] = contents->args[i].isVar() ? contents->args[i] : Var();
        }
        if (!contents->f.rhs().isDefined()) {
            Expr init = cast(e.type(), 0);
            init.addImplicitArgs(e.implicitArgs());
            contents->f.define(gather_args, init);
        }
        contents->f.define(contents->args, contents->f(contents->args) + e);
    }

    void FuncRef::operator*=(const Expr &e) {
        std::vector<Expr> gather_args(contents->args.size());
        for (size_t i = 0; i < gather_args.size(); i++) {
            gather_args[i] = contents->args[i].isVar() ? contents->args[i] : Var();
        }
        if (!contents->f.rhs().isDefined()) {
            Expr init = cast(e.type(), 1);
            init.addImplicitArgs(e.implicitArgs());
            contents->f.define(gather_args, init);
        }
        contents->f.define(contents->args, contents->f(contents->args) * e);
    }

    const Func &FuncRef::f() const {
        return contents->f;
    }

    const std::vector<Expr> &FuncRef::args() const {
        return contents->args;
    }

    Func::Func() : contents(new Contents()) {
    }
 
    Func::Func(const std::string &name) : contents(new Contents(name)) {
    }

    Func::Func(const char *name) : contents(new Contents(name)) {
    }

    Func::Func(const Type &t) : contents(new Contents(t)) {
    }

    Func::Func(const std::string &name, Type t) : contents(new Contents(name, t)) {
    }

    Func::Func(const char *name, Type t) : contents(new Contents(name, t)) {
    }

    bool Func::operator==(const Func &other) const {
        return other.contents == contents;
    }

    const Expr &Func::rhs() const {
        return contents->rhs;
    }

    const Type &Func::returnType() const {
        return contents->returnType;
    }

    const std::vector<Expr> &Func::args() const {
        return contents->args;
    }
    
    const Var &Func::arg(int i) const {
        const Expr& e = args()[i];
        assert (e.isVar());
        return e.vars()[0];
    }

    const std::string &Func::name() const { 
        return contents->name;
    }

    const std::vector<MLVal> &Func::scheduleTransforms() const {
        return contents->scheduleTransforms;
    }

    void Func::define(const std::vector<Expr> &_args, const Expr &r) {
        //printf("Defining %s\n", name().c_str());

        // Make sure the environment exists
        if (!environment) {
            //printf("Creating environment\n");
            environment = new MLVal(makeEnv());
        }

        // Make a local copy of the argument list
        std::vector<Expr> args = _args;

        // Add any implicit arguments 
        //printf("Adding %d implicit arguments\n", r.implicitArgs());
        for (int i = 0; i < r.implicitArgs(); i++) {
            std::ostringstream ss;
            ss << "iv"; // implicit var
            ss << i;
            args.push_back(Var(ss.str()));
        }
        
        //printf("Defining %s\n", name().c_str());

        // Are we talking about a scatter or a gather here?
        bool gather = true;
        //printf("%u args %u rvars\n", (unsigned)args.size(), (unsigned)r.rdom().dimensions());
        for (size_t i = 0; i < args.size(); i++) {            
            if (!args[i].isVar()) {
                gather = false;
            }
        }
        if (r.rdom().dimensions() > 0) gather = false;

        if (gather) {
	    //printf("Gather definition for %s\n", name().c_str());
            contents->rhs = r;            
            contents->returnType = r.type();
            contents->args = args;
            contents->arglist = makeList();
            for (size_t i = args.size(); i > 0; i--) {
                contents->arglist = addToList(contents->arglist, (contents->args[i-1].vars()[0].name()));
            }
             
            contents->definition = makeDefinition((name()), contents->arglist, rhs().node());
            
            *environment = addDefinitionToEnv(*environment, contents->definition);

        } else {
            //printf("Scatter definition for %s\n", name().c_str());
            assert(rhs().isDefined());            

            MLVal update_args = makeList();
            for (size_t i = args.size(); i > 0; i--) {
                update_args = addToList(update_args, args[i-1].node());
                contents->rhs.child(args[i-1]);
            }                                                            

            contents->rhs.child(r);
           
            MLVal reduction_args = makeList();
	    const RDom &rdom = contents->rhs.rdom();
            for (int i = rdom.dimensions(); i > 0; i--) {
                reduction_args = addToList(reduction_args, 
                                           makeTriple(rdom[i-1].name(), 
                                                      rdom[i-1].min().node(), 
                                                      rdom[i-1].size().node()));
            }

            // Make an update function as a handle for scheduling
            contents->update.reset(new Func(uniqueName('p')));
            
            //printf("Adding scatter definition for %s\n", name().c_str());
            // There should already be a gathering definition of this function. Add the scattering term.
            *environment = addScatterToDefinition(*environment, name(), contents->update->name(), 
                                                  update_args, r.node(), reduction_args);


        }
    }

    Func &Func::update() {
        assert(contents->update);
        return *contents->update;
    }

    void *watchdog(void *arg) {
        useconds_t t = ((useconds_t *)arg)[0];
        printf("Watchdog sleeping for %d microseconds\n", t);
        usleep(t);
        printf("Took too long, bailing out\n");
        exit(-1);
    }

    int Func::autotune(int argc, char **argv, std::vector<int> sizes) {
        timeval before, after;

        printf("sizes: ");
        for (size_t i = 0; i < sizes.size(); i++) {
            printf("%u ", sizes[i]);
        }
        printf("\n");

        if (argc == 1) {
            // Run with default schedule to establish baseline timing (including basic compilation time)
            gettimeofday(&before, NULL);
            DynImage im = realize(sizes);
            for (int i = 0; i < 5; i++) realize(im);
            gettimeofday(&after, NULL);            
            useconds_t t = (after.tv_sec - before.tv_sec) * 1000000 + (after.tv_usec - before.tv_usec);
            printf("%d\n", t);
            return 0;
        }        

        // How to schedule it
        for (int i = 2; i < argc; i++) {
            // One random transform per function per arg
            srand(atoi(argv[i]));            
            for (const Func &_f : rhs().funcs()) {
                Func f = _f;
                f.random(rand());
            }
            random(rand());
        }        

        // Set up a watchdog to kill us if we take too long 
        printf("Setting up watchdog timer\n");
        useconds_t time_limit = atoi(argv[1]) + 1000000;
        pthread_t watchdog_thread;
        pthread_create(&watchdog_thread, NULL, watchdog, &time_limit);

        // Trigger compilation and one round of evaluation
        DynImage im = realize(sizes);

        // Start the clock
        gettimeofday(&before, NULL);
        for (int i = 0; i < 5; i++) realize(im);
        gettimeofday(&after, NULL);            
        useconds_t t = (after.tv_sec - before.tv_sec) * 1000000 + (after.tv_usec - before.tv_usec);
        printf("%d\n", t);        
        return 0;
    }

    Func &Func::tile(const Var &x, const Var &y, 
                     const Var &xi, const Var &yi, 
                     const Expr &f1, const Expr &f2) {
        split(x, x, xi, f1);
        split(y, y, yi, f2);
        transpose(x, yi);
        return *this;
    }

    Func &Func::tile(const Var &x, const Var &y, 
                     const Var &xo, const Var &yo,
                     const Var &xi, const Var &yi, 
                     const Expr &f1, const Expr &f2) {
        split(x, xo, xi, f1);
        split(y, yo, yi, f2);
        transpose(xo, yi);
        return *this;
    }

    Func &Func::vectorize(const Var &v) {
        MLVal t = makeVectorizeTransform((name()),
                                         (v.name()));
        contents->scheduleTransforms.push_back(t);
        return *this;
    }

    Func &Func::vectorize(const Var &v, int factor) {
        if (factor == 1) return *this;
        Var vi;
        split(v, v, vi, factor);
        vectorize(vi);        
        return *this;
    }

    Func &Func::unroll(const Var &v) {
        MLVal t = makeUnrollTransform((name()),
                                      (v.name()));        
        contents->scheduleTransforms.push_back(t);
        return *this;
    }

    Func &Func::unroll(const Var &v, int factor) {
        if (factor == 1) return *this;
        Var vi;
        split(v, v, vi, factor);
        unroll(vi);
        return *this;
    }

    Func &Func::split(const Var &old, const Var &newout, const Var &newin, const Expr &factor) {
        MLVal t = makeSplitTransform(name(),
                                     old.name(),
                                     newout.name(),
                                     newin.name(),
                                     factor.node());
        contents->scheduleTransforms.push_back(t);
        return *this;
    }

    Func &Func::transpose(const Var &outer, const Var &inner) {
        MLVal t = makeTransposeTransform((name()),
                                         (outer.name()),
                                         (inner.name()));
        contents->scheduleTransforms.push_back(t);
        return *this;
    }

    Func &Func::chunk(const Var &caller_var) {
        MLVal t = makeChunkTransform(name(), caller_var.name());
        contents->scheduleTransforms.push_back(t);
        return *this;
    }

    Func &Func::root() {
        MLVal t = makeRootTransform(name());
        contents->scheduleTransforms.push_back(t);
        return *this;
    }

    Func &Func::random(int seed) {
        MLVal t = makeRandomTransform(name(), seed);
        contents->scheduleTransforms.push_back(t);
        return *this;
    }

    Func &Func::parallel(const Var &caller_var) {
        MLVal t = makeParallelTransform(name(), caller_var.name());
        contents->scheduleTransforms.push_back(t);
        return *this;
    }

    Func &Func::rename(const Var &oldname, const Var &newname) {
        Var dummy;
        return split(oldname, newname, dummy, 1);
    }

    Func &Func::cuda(const Var &b, const Var &t) {
        Var tidx("threadidx");
        Var bidx("blockidx");
        rename(b, bidx);
        rename(t, tidx);
        parallel(bidx);
        parallel(tidx);
        return *this;
    }

    Func &Func::cuda(const Var &bx, const Var &by, 
                     const Var &tx, const Var &ty) {
        Var tidx("threadidx");
        Var bidx("blockidx");
        Var tidy("threadidy");
        Var bidy("blockidy");
        rename(bx, bidx);
        rename(tx, tidx);
        rename(by, bidy);
        rename(ty, tidy);
        parallel(bidx);
        parallel(bidy);
        parallel(tidx);
        parallel(tidy);
        return *this;
    }


    Func &Func::cudaTile(const Var &x, int xFactor) {
        Var tidx("threadidx");
        Var bidx("blockidx");
        split(x, bidx, tidx, xFactor);
        parallel(bidx);
        parallel(tidx);
        return *this;
    }

    Func &Func::cudaTile(const Var &x, const Var &y, int xFactor, int yFactor) {
        Var tidx("threadidx");
        Var bidx("blockidx");
        Var tidy("threadidy");
        Var bidy("blockidy");
        tile(x, y, bidx, bidy, tidx, tidy, xFactor, yFactor);
        parallel(bidx);
        parallel(tidx);
        parallel(bidy);
        parallel(tidy);
        return *this;
    }

    DynImage Func::realize(int a) {
        DynImage im(returnType(), a);
        realize(im);
        return im;
    }

    DynImage Func::realize(int a, int b) {
        DynImage im(returnType(), a, b);
        realize(im);
        return im;
    }

    DynImage Func::realize(int a, int b, int c) {
        DynImage im(returnType(), a, b, c);
        realize(im);
        return im;
    }


    DynImage Func::realize(int a, int b, int c, int d) {
        DynImage im(returnType(), a, b, c, d);
        realize(im);
        return im;
    }

    DynImage Func::realize(std::vector<int> sizes) {
        DynImage im(returnType(), sizes);
        realize(im);
        return im;
    }


    MLVal Func::Contents::applyScheduleTransforms(MLVal guru) {
        // If we're not inline, obey any tuple shape scheduling hints
        if (scheduleTransforms.size() && rhs.isDefined() && rhs.shape().size()) {
            /*
            printf("%s has tuple shape: ", name.c_str());
            for (size_t i = 0; i < rhs.shape().size(); i++) {
                printf("%d ", rhs.shape()[i]);
            }
            printf("\n");
            */

            for (size_t i = 0; i < rhs.shape().size(); i++) {
                assert(args[args.size()-1-i].isVar());
                // The tuple var is the first implicit var (TODO: this is very very ugly)
                Var t("iv0");
                // Pull all the vars inside the tuple var outside
                bool inside = false;
                for (size_t j = args.size(); j > 0; j--) {
                    assert(args[j-1].isVar());
                    Var x = args[j-1].vars()[0];
                    if (x.name() == t.name()) {
                        inside = true;
                        continue;
                    }
                    if (inside) {
                        //printf("Pulling %s outside of %s\n", x.name().c_str(), t.name().c_str());
                        MLVal trans = makeTransposeTransform(name, x.name(), t.name());
                        guru = trans(guru);
                    }
                }
                assert(inside);

                MLVal trans = makeBoundTransform(name, t.name(), Expr(0).node(), Expr(rhs.shape()[i]).node());
                guru = trans(guru);
                trans = makeUnrollTransform(name, t.name());
                guru = trans(guru);
            }
        }
        for (size_t i = 0; i < scheduleTransforms.size(); i++) {
            guru = scheduleTransforms[i](guru);
        }
        if (update) {
            guru = update->contents->applyScheduleTransforms(guru);
        }
        return guru;
    }

    // Returns a stmt, args pair
    MLVal Func::lower() {
        // Make a region to evaluate this over
        MLVal sizes = makeList();        
        for (size_t i = args().size(); i > 0; i--) {                
            char buf[256];
            snprintf(buf, 256, ".result.dim.%d", ((int)i)-1);
            sizes = addToList(sizes, Expr(Var(buf)).node());
        }

        MLVal guru = makeNoviceGuru();

        // Output is always scheduled root
        root();

        guru = contents->applyScheduleTransforms(guru);

        for (size_t i = 0; i < rhs().funcs().size(); i++) {
            Func f = rhs().funcs()[i];
            // Don't consider recursive dependencies for the
            // purpose of applying schedule transformations. We
            // already did that above.
            if (f == *this) continue;
            guru = f.contents->applyScheduleTransforms(guru);
        }

        //saveGuruToFile(guru, name() + ".guru");
        
        MLVal sched = makeSchedule((name()),
                                   sizes,
                                   *Func::environment,
                                   guru);
        
        //printf("Done transforming schedule\n");
        //printSchedule(sched);
        
        return doLower((name()), 
                       *Func::environment,
                       sched);        
    }

    MLVal Func::inferArguments() {        
        MLVal fargs = makeList();
        fargs = addToList(fargs, makeBufferArg("result"));
        for (size_t i = rhs().uniformImages().size(); i > 0; i--) {
            MLVal arg = makeBufferArg(rhs().uniformImages()[i-1].name());
            fargs = addToList(fargs, arg);
        }
        for (size_t i = rhs().images().size(); i > 0; i--) {
            MLVal arg = makeBufferArg(rhs().images()[i-1].name());
            fargs = addToList(fargs, arg);
        }
        for (size_t i = rhs().uniforms().size(); i > 0; i--) {
            const DynUniform &u = rhs().uniforms()[i-1];
            MLVal arg = makeScalarArg(u.name(), u.type().mlval);
            fargs = addToList(fargs, arg);
        }
        return fargs;
    }

    Func::Arg::Arg(const UniformImage &u) : arg(makeBufferArg(u.name())) {}
    Func::Arg::Arg(const DynUniform &u) : arg(makeScalarArg(u.name(), u.type().mlval)) {}
    Func::Arg::Arg(const DynImage &u) : arg(makeBufferArg(u.name())) {}

    void Func::compileToFile(const std::string &moduleName) { 
        MLVal stmt = lower();
        MLVal args = inferArguments();
        doCompileToFile(moduleName, args, stmt);
    }

    void Func::compileToFile(const std::string &moduleName, std::vector<Func::Arg> uniforms) { 
        MLVal stmt = lower();

        MLVal args = makeList();
        args = addToList(args, makeBufferArg("result"));
        for (size_t i = uniforms.size(); i > 0; i--) {
            args = addToList(args, uniforms[i-1].arg);
        }

        doCompileToFile(moduleName, args, stmt);
    }

    void Func::compileJIT() {
        if (getenv("HL_PSUEDOJIT") && getenv("HL_PSUEDOJIT") == std::string("1")) {
            // llvm's ARM jit path has many issues currently. Instead
            // we'll do static compilation to a shared object, then
            // dlopen it
            printf("Psuedo-jitting via static compilation to a shared object\n");
            std::string name = contents->name + "_psuedojit";
            std::string bc_name = "./" + name + ".bc";
            std::string so_name = "./" + name + ".so";
            std::string obj_name = "./" + name + ".o";
            std::string entrypoint_name = name + "_c_wrapper";
            compileToFile(name.c_str());
            char cmd1[1024], cmd2[1024];
            snprintf(cmd1, 1024, "opt -O3 %s | llc -O3 -relocation-model=pic -filetype=obj > %s", bc_name.c_str(), obj_name.c_str());
            snprintf(cmd2, 1024, "gcc -shared %s -o %s", obj_name.c_str(), so_name.c_str());
            printf("%s\n", cmd1);
            assert(0 == system(cmd1));
            printf("%s\n", cmd2);
            assert(0 == system(cmd2));
            void *handle = dlopen(so_name.c_str(), RTLD_NOW);
            assert(handle);
            void *ptr = dlsym(handle, entrypoint_name.c_str());
            assert(ptr);
            contents->functionPtr = (void (*)(void *))ptr;
            return;
        }

        if (!Contents::ee) {
            llvm::InitializeNativeTarget();
        }

        MLVal stmt = lower();
        MLVal args = inferArguments();

        printf("compiling IR -> ll\n");
        MLVal tuple;
        tuple = doCompile(name(), args, stmt);

        printf("Extracting the resulting module and function\n");
        MLVal first, second;
        MLVal::unpackPair(tuple, first, second);
        LLVMModuleRef module = (LLVMModuleRef)(first.asVoidPtr());
        LLVMValueRef func = (LLVMValueRef)(second.asVoidPtr());
        llvm::Function *f = llvm::unwrap<llvm::Function>(func);
        llvm::Module *m = llvm::unwrap(module);

        if (!Contents::ee) {
            std::string errStr;
            Contents::ee = llvm::EngineBuilder(m).setErrorStr(&errStr).setOptLevel(llvm::CodeGenOpt::Aggressive).create();
            if (!contents->ee) {
                printf("Couldn't create execution engine: %s\n", errStr.c_str()); 
                exit(1);
            }

            // Set up the pass manager
            Contents::fPassMgr = new llvm::FunctionPassManager(m);
            Contents::mPassMgr = new llvm::PassManager();

            llvm::PassManagerBuilder builder;
            builder.OptLevel = 3;
            builder.populateFunctionPassManager(*contents->fPassMgr);
            builder.populateModulePassManager(*contents->mPassMgr);

        } else { 
            Contents::ee->addModule(m);
        }            
        
        std::string functionName = name() + "_c_wrapper";
        llvm::Function *inner = m->getFunction(functionName.c_str());
        
        // Remap the cuda_ctx of PTX host modules to a shared location for all instances.
        // CUDA behaves much better when you don't initialize >2 contexts.
        llvm::GlobalVariable* ctx = m->getNamedGlobal("cuda_ctx");
        if (ctx) {
            Contents::ee->addGlobalMapping(ctx, (void*)&cuda_ctx);
        }
        
        if (!inner) {
            printf("Could not find function %s", functionName.c_str());
            exit(1);
        }
        
        printf("optimizing ll...\n");
        
        std::string errstr;
        llvm::raw_fd_ostream stdout("passes.txt", errstr);
        
        Contents::mPassMgr->run(*m);

        Contents::fPassMgr->doInitialization();
        
        Contents::fPassMgr->run(*inner);
        
        Contents::fPassMgr->doFinalization();
        
        printf("compiling ll -> machine code...\n");
        void *ptr = Contents::ee->getPointerToFunction(f);
        contents->functionPtr = (void (*)(void*))ptr;
        
        llvm::Function *copyToHost = m->getFunction("__copy_to_host");
        if (copyToHost) {
            ptr = Contents::ee->getPointerToFunction(copyToHost);
            contents->copyToHost = (void (*)(buffer_t*))ptr;
        }
        
        llvm::Function *freeBuffer = m->getFunction("__free_buffer");
        if (freeBuffer) {
            ptr = Contents::ee->getPointerToFunction(freeBuffer);
            contents->freeBuffer = (void (*)(buffer_t*))ptr;
        }       
    }

    size_t im_size(const DynImage &im, int dim) {
        return im.size(dim);
    }
    
    size_t im_size(const UniformImage &im, int dim) {
        return im.boundImage().size(dim);
    }

    void Func::realize(const DynImage &im) {
        if (!contents->functionPtr) compileJIT();

        //printf("Constructing argument list...\n");
        void *arguments[256];
        buffer_t *buffers[256];
        size_t j = 0;
        size_t k = 0;

        for (size_t i = 0; i < rhs().uniforms().size(); i++) {
            arguments[j++] = rhs().uniforms()[i].data();
        }
        for (size_t i = 0; i < rhs().images().size(); i++) {
            buffers[k++] = rhs().images()[i].buffer();
            arguments[j++] = buffers[k-1];
        }               
        for (size_t i = 0; i < rhs().uniformImages().size(); i++) {
            buffers[k++] = rhs().uniformImages()[i].boundImage().buffer();
            arguments[j++] = buffers[k-1];
        }
        buffers[k] = im.buffer();
        arguments[j] = buffers[k];

        /*
        printf("Args: ");
        for (size_t i = 0; i <= j; i++) {
            printf("%p ", arguments[i]);
        }
        printf("\n");

        printf("Calling function at %p\n", contents->functionPtr); 
        */
        contents->functionPtr(&arguments[0]);
        
        if (use_gpu()) {
            assert(contents->copyToHost);
            im.setRuntimeHooks(contents->copyToHost, contents->freeBuffer);
        }
        
        // TODO: the actual codegen entrypoint should probably set this for x86/ARM targets too
        if (!im.devDirty()) {
            im.markHostDirty();
        }
    }

    MLVal *Func::environment = NULL;

}
