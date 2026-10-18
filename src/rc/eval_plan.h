/* Optional, bounded preparation of immutable interpreted bodies. This is a
 * private representation, never serialized or accepted from a public value.
 * Every managed literal is a closure edge; the code string contains indices.
 * A plan never captures its own AST root. Pair caches therefore add no new
 * vector reachability or immutable backedges to the collector graph. */
typedef enum { P_CONST, P_LOCAL, P_NONLOCAL, P_CALL, P_CALL_EXPR, P_IF,
    P_LET, P_COND, P_AND, P_OR, P_TYPE, P_FALLBACK } PlanOp;
typedef struct { uint16_t op,a,b,c,n; } PlanNode;
typedef struct { uint32_t nodes,params,literals,aux,root; } PlanHeader;
typedef struct PlanBinding {
    RcValue *name; size_t depth; const struct PlanBinding *outer;
} PlanBinding;
#define PLAN_LIMIT 4096u
#define PLAN_DEPTH 128u
typedef struct {
    RcContext *ctx; RcValue *const *params; size_t arity;
    PlanNode *nodes; uint16_t *aux; RcValue **literals;
    size_t nn,na,nl; bool failed;
} PlanBuild;
static RcValue *plan_marker(RcContext *ctx,RcValue *self,size_t argc,RcValue *const *argv) {
    (void)self;(void)argc;(void)argv;
    return rc_error(ctx,"eval-kl: private evaluation plan is not callable");
}
static uint32_t plan_literal(PlanBuild *b,RcValue *v) {
    /* Common symbols/literals share a capture. This bound also keeps optional
     * preparation from retaining arbitrarily large compiler scratch buffers. */
    for(size_t i=0;i<b->nl;i++)if(b->literals[i]==v)return (uint32_t)i;
    if(b->nl==PLAN_LIMIT){b->failed=true;return 0;}
    b->literals[b->nl]=v;return (uint32_t)b->nl++;
}
static bool plan_local(PlanBuild *b,RcValue *name,const PlanBinding *scope,
                       size_t depth,uint16_t *up,uint16_t *slot) {
    for(const PlanBinding *p=scope;p;p=p->outer)if(p->name==name) {
        *up=(uint32_t)(depth-p->depth);*slot=0;return true;
    }
    for(size_t i=0;i<b->arity;i++)if(b->params[i]==name) {
        *up=(uint32_t)depth;*slot=(uint32_t)i;return true;
    }
    return false;
}
static uint32_t plan_compile(PlanBuild *b,RcValue *expr,const PlanBinding *scope,
                             size_t depth,size_t recursion) {
    if(b->failed || recursion==PLAN_DEPTH || b->nn==PLAN_LIMIT) {
        b->failed=true;return 0;
    }
    uint32_t id=(uint32_t)b->nn++;PlanNode *node=&b->nodes[id];
    *node=(PlanNode){P_CONST,0,0,0,0};
    if(rc_type(expr)!=RC_PAIR) {
        if(rc_type(expr)==RC_SYMBOL) {
            if(plan_local(b,expr,scope,depth,&node->a,&node->b))node->op=P_LOCAL;
            else {node->op=P_NONLOCAL;node->a=plan_literal(b,expr);}
        } else node->a=plan_literal(b,expr);
        return id;
    }
    RcValue *head=rc_head_borrowed(expr),*args=rc_tail_borrowed(expr);
    size_t n;uint16_t up,slot;
    if(rc_list_length_cached(args,&n)!=RC_LIST_LENGTH_OK || n>PLAN_LIMIT)goto fallback;
    Form form=rc_type(head)==RC_SYMBOL && plan_local(b,head,scope,depth,&up,&slot)
        ? F_CALL : form_of(head);
    if(form==F_IF) {
        if(n!=3)goto fallback;
        node->op=P_IF;
        node->a=plan_compile(b,nth(args,0),scope,depth,recursion+1);
        node->b=plan_compile(b,nth(args,1),scope,depth,recursion+1);
        node->c=plan_compile(b,nth(args,2),scope,depth,recursion+1);
    } else if(form==F_LET) {
        if(n!=3 || rc_type(nth(args,0))!=RC_SYMBOL)goto fallback;
        node->op=P_LET;node->a=plan_literal(b,nth(args,0));
        node->b=plan_compile(b,nth(args,1),scope,depth,recursion+1);
        PlanBinding binding={nth(args,0),depth+1,scope};
        node->c=plan_compile(b,nth(args,2),&binding,depth+1,recursion+1);
    } else if(form==F_COND) {
        if(n>(PLAN_LIMIT-b->na)/2)goto fallback;
        /* Validate clause shapes without evaluating hidden branches. A bad
         * clause keeps the entire cond on the original lazy validation path. */
        for(RcValue *p=args;!rc_is_nil(p);p=rc_tail_borrowed(p)) {
            size_t count;
            if(rc_list_length_cached(rc_head_borrowed(p),&count)!=RC_LIST_LENGTH_OK || count!=2)goto fallback;
        }
        node->op=P_COND;node->a=(uint32_t)b->na;node->n=(uint32_t)n;b->na+=2*n;
        for(size_t i=0;i<n;i++,args=rc_tail_borrowed(args)) {
            RcValue *clause=rc_head_borrowed(args);
            b->aux[node->a+2*i]=plan_compile(b,nth(clause,0),scope,depth,recursion+1);
            b->aux[node->a+2*i+1]=plan_compile(b,nth(clause,1),scope,depth,recursion+1);
        }
    } else if(form==F_AND || form==F_OR) {
        if(n!=2)goto fallback;
        node->op=form==F_AND ? P_AND : P_OR;
        node->a=plan_compile(b,nth(args,0),scope,depth,recursion+1);
        node->b=plan_compile(b,nth(args,1),scope,depth,recursion+1);
    } else if(form==F_TYPE) {
        if(n!=2)goto fallback;
        node->op=P_TYPE;node->a=plan_compile(b,nth(args,0),scope,depth,recursion+1);
    } else if(form==F_QUOTE) {
        if(n!=1)goto fallback;
        node->a=plan_literal(b,nth(args,0));
    } else if(form==F_CALL) {
        if(n>PLAN_LIMIT-b->na)goto fallback;
        node->op=rc_type(head)==RC_SYMBOL ? P_CALL : P_CALL_EXPR;
        node->a=plan_compile(b,head,scope,depth,recursion+1);
        node->b=(uint32_t)b->na;node->n=(uint32_t)n;b->na+=n;
        for(size_t i=0;i<n;i++,args=rc_tail_borrowed(args))
            b->aux[node->b+i]=plan_compile(b,rc_head_borrowed(args),scope,depth,recursion+1);
    } else goto fallback;
    return id;
fallback:
    node->op=P_FALLBACK;node->a=plan_literal(b,expr);return id;
}
static const PlanHeader *plan_header(RcValue *plan) {
    return (const PlanHeader *)(const void *)rc_text(rc_capture(plan,0));
}
static RcValue *plan_prepare(RcContext *ctx,RcValue *body,RcValue *environment,
                             size_t arity,RcValue *const *params,bool executing) {
    if(rc_type(body)!=RC_PAIR || arity>PLAN_LIMIT || !rc_eval_plan_environment(environment))return NULL;
    RcValue *old=rc_eval_plan_get(body);
    if(old && rc_closure_is_native(old,plan_marker)) {
        const PlanHeader *h=plan_header(old);bool match=h->params==arity;
        for(size_t i=0;match && i<arity;i++)match=rc_capture(old,i+1)==params[i];
        if(match)return rc_retain(old);
    }
    if(!executing || !rc_eval_plan_heat(body))return NULL;
    /* Unsupported roots cannot be cached: capturing body would make a cycle.
     * Reject them before allocating optional compiler scratch storage. */
    Form root_form=form_of(rc_head_borrowed(body));
    if(root_form==F_LAMBDA || root_form==F_FREEZE || root_form==F_DEFUN || root_form==F_TRAP)return NULL;
    PlanBuild b={.ctx=ctx,.params=params,.arity=arity};
    b.nodes=malloc(PLAN_LIMIT*sizeof(*b.nodes));
    b.aux=malloc(PLAN_LIMIT*sizeof(*b.aux));
    b.literals=malloc(PLAN_LIMIT*sizeof(*b.literals));
    RcValue *plan=NULL,*code=NULL;RcValue **captures=NULL;char *bytes=NULL;
    if(!b.nodes || !b.aux || !b.literals)goto done;
    uint32_t root=plan_compile(&b,body,NULL,0,0);
    if(b.failed || b.nodes[root].op==P_FALLBACK)goto done;
    size_t size=sizeof(PlanHeader)+b.nn*sizeof(PlanNode)+b.na*sizeof(uint16_t);
    bytes=malloc(size);captures=malloc((1+arity+b.nl)*sizeof(*captures));
    if(!bytes || !captures)goto done;
    PlanHeader h={(uint32_t)b.nn,(uint32_t)arity,(uint32_t)b.nl,(uint32_t)b.na,root};
    memcpy(bytes,&h,sizeof(h));memcpy(bytes+sizeof(h),b.nodes,b.nn*sizeof(PlanNode));
    memcpy(bytes+sizeof(h)+b.nn*sizeof(PlanNode),b.aux,b.na*sizeof(uint16_t));
    code=rc_string_n(ctx,bytes,size);if(!code)goto done;
    captures[0]=code;
    for(size_t i=0;i<arity;i++)captures[i+1]=params[i];
    for(size_t i=0;i<b.nl;i++)captures[i+1+arity]=b.literals[i];
    plan=rc_closure(ctx,0,plan_marker,1+arity+b.nl,captures);
    if(plan)rc_eval_plan_set(body,plan);
done:
    free(b.nodes);free(b.aux);free(b.literals);free(captures);free(bytes);rc_release(code);
    return plan;
}
typedef struct PlanEnvironment {
    struct PlanEnvironment *parent;
    RcValue *name,*value,*materialized;
} PlanEnvironment;
typedef struct {
    RcValue *plan,*parent,*materialized;
    RcValue *const *params,*const *argv;size_t arity;
    const PlanNode *nodes;const uint16_t *aux;size_t literal_base;
} PlanRun;
/* Runtime dispatch owns every borrowed callback argument until return. Local
 * values have one C owner. Only generic evaluation/closure escape needs heap
 * frames; materialize and root those exact frames lazily on that path. */
static RcValue *plan_environment(RcContext *ctx,PlanRun *run,PlanEnvironment *env) {
    if(!env) {
        if(!run->materialized)run->materialized=rc_env_frame(ctx,run->parent,run->arity,run->params,run->argv);
        return run->materialized;
    }
    if(!env->materialized) {
        RcValue *parent=plan_environment(ctx,run,env->parent);
        if(parent)env->materialized=extend(ctx,parent,env->name,env->value);
    }
    return env->materialized;
}
static inline RcValue *plan_value(const PlanRun *run,const PlanNode *node,PlanEnvironment *env) {
    if(node->op==P_LOCAL) {
        for(size_t i=0;i<node->a;i++)env=env->parent;
        return env ? env->value : run->argv[node->b];
    }
    RcValue *literal=rc_capture(run->plan,run->literal_base+node->a);
    RcValue *lexical=node->op==P_NONLOCAL ? lookup(run->parent,literal) : NULL;
    return lexical ? lexical : literal;
}
static RcValue *plan_evaluate(RcContext *,PlanRun *,uint32_t,PlanEnvironment *,bool);
static inline RcValue *plan_argument(RcContext *ctx,PlanRun *run,uint32_t pc,PlanEnvironment *env,bool tail) {
    const PlanNode *node=&run->nodes[pc];
    return node->op<=P_NONLOCAL ? rc_retain(plan_value(run,node,env))
        : plan_evaluate(ctx,run,pc,env,tail);
}
static RcValue *plan_application(RcContext *ctx,PlanRun *run,const PlanNode *node,PlanEnvironment *env,bool tail) {
    const PlanNode *head=&run->nodes[node->a];
    RcValue *function;
    if(node->op==P_CALL) {
        RcValue *value=plan_value(run,head,env);
        function=rc_type(value)==RC_SYMBOL ? rc_function_get(ctx,value) : rc_retain(value);
    } else {
        function=plan_argument(ctx,run,node->a,env,false);
        if(function && rc_type(function)==RC_SYMBOL) {
            RcValue *resolved=rc_function_get(ctx,function);rc_release(function);function=resolved;
        }
    }
    if(!function)return NULL;
    const uint16_t *args=run->aux+node->b;size_t argc=node->n;
    if(tail && argc==2 && rc_closure_returns_second(ctx,function)) {
        RcValue *ignored=plan_argument(ctx,run,args[0],env,false);
        if(!ignored){rc_release(function);return NULL;}
        bool valid_first=rc_call_values_belong(ctx,function,1,&ignored);
        RcValue *result=plan_argument(ctx,run,args[1],env,valid_first);
        if(result && !valid_first) {
            RcValue *argv[2]={ignored,result};
            result=rc_primitive_call_rooted_owned(ctx,function,2,argv,true);
            rc_release(argv[0]);rc_release(argv[1]);rc_release(function);return result;
        }
        rc_release(ignored);rc_release(function);
        if(!result)return NULL;
        if(rc_has_error(ctx)){rc_release(result);return NULL;}
        result=rc_eval_require_argument_context(ctx,result);
        if(result)rc_safepoint(ctx);return result;
    }
    RcValue *small[8],**argv=argc<=8 ? small : calloc(argc,sizeof(*argv));
    if(!argv){rc_release(function);return rc_error(ctx,"eval-kl: out of memory");}
    RcValue *result=NULL;size_t i=0;
    for(;i<argc;i++){argv[i]=plan_argument(ctx,run,args[i],env,false);if(!argv[i])break;}
    if(i==argc)result=rc_primitive_call_rooted_owned(ctx,function,argc,argv,tail);
    for(size_t j=0;j<i;j++)rc_release(argv[j]);
    if(argv!=small)free(argv);rc_release(function);return result;
}
static RcValue *plan_evaluate(RcContext *ctx,PlanRun *run,uint32_t pc,PlanEnvironment *environment,bool tail) {
    PlanEnvironment *env=environment;RcValue *result=NULL;
    for(;;) {
        const PlanNode *node=&run->nodes[pc];
        switch(node->op) {
        case P_CONST:case P_LOCAL:case P_NONLOCAL:
            result=rc_retain(plan_value(run,node,env));goto done;
        case P_CALL:case P_CALL_EXPR:
            result=plan_application(ctx,run,node,env,tail);goto done;
        case P_IF: {
            RcValue *test=plan_argument(ctx,run,node->a,env,false);if(!test)goto done;
            bool yes=rc_is_true(test),valid=boolean(test);rc_release(test);
            if(!valid){rc_error(ctx,"eval-kl: if test must be boolean");goto done;}
            pc=yes ? node->b : node->c;break;
        }
        case P_LET: {
            RcValue *value=plan_argument(ctx,run,node->b,env,false);if(!value)goto done;
            PlanEnvironment next={env,rc_capture(run->plan,run->literal_base+node->a),value,NULL};
            result=plan_argument(ctx,run,node->c,&next,tail);
            rc_release(next.materialized);rc_release(value);goto done;
        }
        case P_COND: {
            bool found=false;
            for(size_t i=0;i<node->n;i++) {
                RcValue *test=plan_argument(ctx,run,run->aux[node->a+2*i],env,false);if(!test)goto done;
                bool yes=rc_is_true(test),valid=boolean(test);rc_release(test);
                if(!valid){rc_error(ctx,"eval-kl: cond test must be boolean");goto done;}
                if(yes){pc=run->aux[node->a+2*i+1];found=true;break;}
            }
            if(!found){rc_error(ctx,"eval-kl: no true cond case");goto done;}break;
        }
        case P_AND:case P_OR: {
            bool is_and=node->op==P_AND;
            RcValue *first=plan_argument(ctx,run,node->a,env,false);if(!first)goto done;
            if(!boolean(first)){rc_release(first);rc_error(ctx,"eval-kl: boolean arguments required");goto done;}
            if(rc_is_true(first)!=is_and){result=first;goto done;}rc_release(first);
            result=rc_require_boolean(ctx,plan_argument(ctx,run,node->b,env,tail));goto done;
        }
        case P_TYPE:pc=node->a;break;
        case P_FALLBACK: {
            RcValue *frame=plan_environment(ctx,run,env);
            if(frame)result=evaluate(ctx,rc_capture(run->plan,run->literal_base+node->a),frame,tail);
            goto done;
        }
        }
    }
done:
    return result;
}
static RcValue *prepared_call(RcContext *ctx,RcValue *self,size_t argc,RcValue *const *argv) {
    RcValue *const *captures=rc_eval_captures(ctx,self,prepared_call,argc);if(!captures)return NULL;
    RcValue *plan=captures[0];const PlanHeader *h=plan_header(plan);
    const PlanNode *nodes=(const PlanNode *)(const void *)(h+1);
    PlanRun run={.plan=plan,.parent=captures[1],.params=captures+2,.argv=argv,.arity=argc,
        .nodes=nodes,.aux=(const uint16_t *)(const void *)(nodes+h->nodes),.literal_base=1+h->params};
    RcValue *result=plan_evaluate(ctx,&run,h->root,NULL,true);rc_release(run.materialized);return result;
}
