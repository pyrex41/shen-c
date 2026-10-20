"""Fail-closed evidence extraction for the RC emitter's generated C format."""
import ast
import hashlib
import re


def _code_mask(source):
    """Preserve offsets and code punctuation while hiding literals/comments."""
    mask = list(source); literals = []; index = 0
    while index < len(source):
        start = index
        if source[index] in ('"', "'"):
            quote = source[index]; index += 1
            while index < len(source):
                if source[index] == '\\':
                    index += 2
                elif source[index] == quote:
                    index += 1; break
                else:
                    index += 1
            else:
                raise ValueError('unclosed generated literal')
            literals.append((start, index, source[start:index]))
        elif source.startswith('//', index):
            newline = source.find('\n', index)
            index = len(source) if newline < 0 else newline
        elif source.startswith('/*', index):
            close = source.find('*/', index + 2)
            if close < 0:
                raise ValueError('unclosed generated comment')
            index = close + 2
        else:
            index += 1; continue
        for offset in range(start, min(index, len(source))):
            if source[offset] != '\n':
                mask[offset] = ' '
    return ''.join(mask), literals


def _cstring(token):
    # The emitter octal-escapes non-ASCII UTF-8 bytes in C string literals.
    return ast.literal_eval(token).encode('latin1').decode('utf-8', 'surrogateescape')


def _bodies(source):
    result = {}; mask, _ = _code_mask(source)
    pattern = r'static\s+RcValue\s*\*\s*(rc_native_(?:body_)?\d+)\s*\([^;{}]*\)\s*\{'
    for match in re.finditer(pattern, mask):
        name = match[1]
        if name in result:
            raise ValueError('duplicate native definition: ' + name)
        start = match.end(); end = start; depth = 1
        while end < len(mask) and depth:
            depth += (mask[end] == '{') - (mask[end] == '}')
            end += 1
        if depth:
            raise ValueError('missing native body boundary: ' + name)
        result[name] = source[start:end - 1]
    if not result:
        raise ValueError('no native C function bodies')
    return result


def _symbols(source):
    mask, literals = _code_mask(source)
    table = re.search(r'static const char \*const rc_module_symbol_names\[\]\s*=\s*\{(.*?)\};', mask, re.S)
    if table is None:
        return []
    if re.sub(r'[\s,]', '', table[1]):
        raise ValueError('unknown generated symbol table format')
    tokens = [token for start, end, token in literals
              if table.start(1) <= start and end <= table.end(1)]
    if any(not token.startswith('"') for token in tokens):
        raise ValueError('non-string module symbol')
    return [_cstring(token) for token in tokens]


def _symbol_value(expression, names):
    # Immortal symbols are loaded directly; older output retained them.
    cached = re.fullmatch(r'(?:rc_retain\(symbols\[(\d+)\]\)|symbols\[(\d+)\])', expression.strip())
    if cached:
        index = int(cached[1] or cached[2])
        if index >= len(names):
            raise ValueError('compiled symbol index out of range')
        return names[index]
    literal = re.fullmatch(r'rc_symbol\(ctx,("(?:\\.|[^"\\])*")\)', expression.strip())
    return _cstring(literal[1]) if literal else None


def native_definition_evidence(source, symbol=None):
    bodies = _bodies(source); names = _symbols(source)
    selected = source; callback = body_name = None
    if symbol is not None:
        registrations = []
        for body in bodies.values():
            slots = {}; cursor = 0; mask, _ = _code_mask(body)
            for setter in re.finditer(r'rc_function_set\(ctx,t\[(\d+)\],t\[(\d+)\]\)', mask):
                for assignment in re.finditer(r't\[(\d+)\]\s*=\s*([^;\n]+);', mask[cursor:setter.start()]):
                    start, end = assignment.span(2)
                    slots[assignment[1]] = body[cursor + start:cursor + end].strip()
                cursor = setter.end()
                registered = _symbol_value(slots.get(setter[1], ''), names)
                if registered is None:
                    raise ValueError('unknown generated symbol registration')
                constructor = slots.get(setter[2], '')
                # Top-level definitions wrap the constructor to make it immortal.
                static = re.fullmatch(r'rc_closure_static\((.*)\)', constructor)
                constructor = static[1] if static else constructor
                closure = re.fullmatch(r'rc_closure(?:_owned)?\(ctx,(\d+),(rc_native_\d+),\d+,[^;]+\)', constructor)
                identity = re.fullmatch(r'rc_closure_return_second(?:_owned)?\(ctx,(rc_native_\d+),\d+,[^;]+\)', constructor)
                if closure:
                    target, target_arity = closure[2], int(closure[1])
                elif identity:
                    target, target_arity = identity[1], 2
                else:
                    raise ValueError('unknown callback registration for ' + registered)
                if registered == symbol:
                    registrations.append((target, target_arity))
        if len(registrations) != 1:
            raise ValueError('registration must identify exactly one callback for ' + symbol)
        callback, arity = registrations[0]
        if callback not in bodies:
            raise ValueError('registered callback body missing')
        selected = bodies[callback]; body_name = callback
        selected_mask, _ = _code_mask(selected)
        if re.search(r'\brc_native_body_\d+\s*\(', selected_mask):
            wrapper = re.fullmatch(r'\s*if\(argc!=(\d+)\) return rc_error\(ctx,"native arity mismatch"\);'
                                   r'\s*return (rc_native_body_\d+)\(ctx,self,argv\);\s*', selected)
            if not wrapper or int(wrapper[1]) != arity:
                raise ValueError('unknown or unchecked native wrapper')
            body_name = wrapper[2]
            if body_name != callback.replace('rc_native_', 'rc_native_body_', 1) or body_name not in bodies:
                raise ValueError('native wrapper does not identify its unique body')
            selected = bodies[body_name]
        else:
            guards = list(re.finditer(r'if\(argc!=' + str(arity) + r'\) return rc_error\(ctx,\s*\);', selected_mask))
            expected = 'if(argc!=%d) return rc_error(ctx,"native arity mismatch");' % arity
            if not any(selected[guard.start():guard.end()] == expected for guard in guards):
                raise ValueError('native callback arity guard missing')
        operations = re.findall(r'\bt\[\d+\]\s*=\s*([^;\n]+);', _code_mask(selected)[0])
        if not any(operation.strip() != 'NULL' for operation in operations):
            raise ValueError('native body has no generated value operations')
    used_names = []; selected_mask, _ = _code_mask(selected)
    indices = re.findall(r'\bsymbols\[(\d+)\]', selected_mask)
    indices += re.findall(r'\brc_module_call_(\d+)\s*\(', selected_mask)
    for index in indices:
        if int(index) >= len(names):
            raise ValueError('native body symbol index out of range')
        used_names.append(names[int(index)])
    for call in re.finditer(r'\brc_symbol\(ctx,', selected_mask):
        token = re.match(r'"(?:\\.|[^"\\])*"', selected[call.end():].lstrip())
        if token:
            used_names.append(_cstring(token[0]))
    if re.search(r'\b(?:rc_eval\w*|eval_kl\w*|shen_eval\w*)\s*\(', selected_mask) or 'eval-kl' in used_names:
        raise ValueError('source-evaluator forwarding body')
    return {'callback': callback, 'body': body_name,
            'body_sha256': hashlib.sha256(selected.encode()).hexdigest()}
