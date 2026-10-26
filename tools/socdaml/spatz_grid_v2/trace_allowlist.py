"""Only inspected milestone/completion channels; never /exec or /insn."""
import ctypes

TRACES = (
    '^/chip/cluster_[0-9][0-9]*/loader/trace$',
    '^/chip/cluster_[0-9][0-9]*/pe[012]/exception$',
    '^/chip/cluster_[0-9][0-9]*/cluster_registers/trace$',
    '^/chip/cluster_[0-9][0-9]*/idma/fe/trace$',
    '^/chip/cluster_[0-9][0-9]*/pe0/spatz/trace$',
)
ENV_KEYS = ('PATH','PYTHONPATH','LD_LIBRARY_PATH','DYLD_LIBRARY_PATH',
            'DYLD_FALLBACK_LIBRARY_PATH','DYLD_INSERT_LIBRARIES','LANG','LC_ALL',
            'LC_NUMERIC','LC_TIME','TZ','OMP_NUM_THREADS','SYSTEMC_HOME',
            'SOCDAML_DRAMSYS_LIBRARY','PYTHONDONTWRITEBYTECODE')


def verify():
    libc = ctypes.CDLL(None)
    libc.regcomp.argtypes = (ctypes.c_void_p,ctypes.c_char_p,ctypes.c_int)
    libc.regexec.argtypes = (ctypes.c_void_p,ctypes.c_char_p,ctypes.c_size_t,ctypes.c_void_p,ctypes.c_int)
    libc.regfree.argtypes = (ctypes.c_void_p,)
    forbidden = ('exec','insn','decoder','fetch','clock','memory','wrapper')
    paths = [f'/chip/cluster_{c}/pe{p}/{kind}' for c in range(16) for p in range(3)
             for kind in (*forbidden,'exception')]
    paths += [f'/chip/cluster_{c}/{kind}' for c in range(16)
              for kind in ('loader/trace','cluster_registers/trace','idma/fe/trace','pe0/spatz/trace')]
    coverage = []
    for pattern in TRACES:
        regex = (ctypes.c_long*512)()
        if libc.regcomp(regex, pattern.encode(), 0):
            raise ValueError('Invalid pinned-engine POSIX basic regex: '+pattern)
        try:
            matches = [p for p in paths if libc.regexec(regex,p.encode(),0,None,0)==0]
            if any(p.endswith('/'+kind) for p in matches for kind in forbidden):
                raise ValueError('Forbidden continuous trace selection.')
            expected = 48 if '/pe[012]/' in pattern else 16
            if len(matches)!=expected:
                raise ValueError('Incomplete trace coverage: '+pattern)
            coverage.append(dict(pattern=pattern,paths=len(matches)))
        finally:
            libc.regfree(regex)
    return dict(syntax='POSIX basic',regcomp_flags=0,coverage=coverage)
