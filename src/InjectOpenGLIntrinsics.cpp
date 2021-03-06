#include "InjectOpenGLIntrinsics.h"
#include "IRMutator.h"
#include "IROperator.h"
#include "CodeGen_GPU_Dev.h"
#include "Substitute.h"

namespace Halide {
namespace Internal {

using std::string;
using std::vector;

class InjectOpenGLIntrinsics : public IRMutator {
public:
    InjectOpenGLIntrinsics()
        : inside_kernel_loop(false) {
    }
    Scope<int> scope;
    bool inside_kernel_loop;
private:
    using IRMutator::visit;

    void visit(const Provide *provide) {
        if (!inside_kernel_loop) {
            IRMutator::visit(provide);
            return;
        }

        internal_assert(provide->values.size() == 1) << "GLSL currently only supports single-valued stores.\n";
        user_assert(provide->args.size() == 3) << "GLSL stores requires three coordinates.\n";

        // Create glsl_texture_store(name, name.buffer, x, y, c, value)
        // intrinsic.
        vector<Expr> args(6);
        args[0] = provide->name;
        args[1] = Variable::make(Handle(), provide->name + ".buffer");
        for (size_t i = 0; i < provide->args.size(); i++) {
            args[i + 2] = provide->args[i];
        }
        args[5] = mutate(provide->values[0]);
        stmt = Evaluate::make(
            Call::make(args[5].type(), Call::glsl_texture_store, args, Call::Intrinsic));
    }

    void visit(const Call *call) {
        if (!inside_kernel_loop || call->call_type == Call::Intrinsic) {
            IRMutator::visit(call);
            return;
        }

        string name = call->name;
        if (call->call_type == Call::Halide && call->func.outputs() > 1) {
            name = name + '.' + int_to_string(call->value_index);
        }

        user_assert(call->args.size() == 3) << "GLSL loads requires three coordinates.\n";

        // Create glsl_texture_load(name, name.buffer, x, y, c) intrinsic.
        vector<Expr> args(5);
        args[0] = call->name;
        args[1] = Variable::make(Handle(), call->name + ".buffer");
        for (size_t i = 0; i < call->args.size(); i++) {
            string d = int_to_string(i);
            string min_name = name + ".min." + d;
            string min_name_constrained = min_name + ".constrained";
            if (scope.contains(min_name_constrained)) {
                min_name = min_name_constrained;
            }
            string extent_name = name + ".extent." + d;
            string extent_name_constrained = extent_name + ".constrained";
            if (scope.contains(extent_name_constrained)) {
                extent_name = extent_name_constrained;
            }

            Expr min = Variable::make(Int(32), min_name);
            Expr extent = Variable::make(Int(32), extent_name);

            // Normalize the two spatial coordinates x,y
            args[i + 2] = (i < 2)
                ? (Cast::make(Float(32), call->args[i] - min) + 0.5f) / extent
                : call->args[i] - min;
        }

        expr = Call::make(call->type, Call::glsl_texture_load,
                          args, Call::Intrinsic,
                          Function(), 0, call->image, call->param);
    }

    void visit(const LetStmt *let) {
        // Discover constrained versions of things.
        bool constrained_version_exists = ends_with(let->name, ".constrained");
        if (constrained_version_exists) {
            scope.push(let->name, 0);
        }

        IRMutator::visit(let);

        if (constrained_version_exists) {
            scope.pop(let->name);
        }
    }

    void visit(const For *loop) {
        bool old_kernel_loop = inside_kernel_loop;
        if (loop->for_type == For::Parallel &&
            CodeGen_GPU_Dev::is_gpu_block_var(loop->name)) {
            inside_kernel_loop = true;
        }
        IRMutator::visit(loop);
        inside_kernel_loop = old_kernel_loop;
    }
};

// Rewrite all GPU loops to have a min of zero
class ZeroGPULoopMins : public IRMutator {
    using IRMutator::visit;

    void visit(const For *op) {
        IRMutator::visit(op);
        if (CodeGen_GPU_Dev::is_gpu_var(op->name) && !is_zero(op->min)) {
            op = stmt.as<For>();
            internal_assert(op);
            Expr adjusted = Variable::make(Int(32), op->name) + op->min;
            Stmt body = substitute(op->name, adjusted, op->body);
            stmt = For::make(op->name, 0, op->extent, op->for_type, body);
        }
    }
};

Stmt inject_opengl_intrinsics(Stmt s) {
    ZeroGPULoopMins z;
    s = z.mutate(s);
    InjectOpenGLIntrinsics gl;
    return gl.mutate(s);
}

}
}
