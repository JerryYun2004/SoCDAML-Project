// Native dependency load check; does not validate DRAM timing or memory behavior.
#include <systemc>
#include <dlfcn.h>
#include <cstdio>

SC_MODULE(Tick) {
    void run() { wait(1, sc_core::SC_NS); sc_core::sc_stop(); }
    SC_CTOR(Tick) { SC_THREAD(run); }
};

int sc_main(int argc, char **argv) {
    if (argc != 2) return 2;
    void *library = dlopen(argv[1], RTLD_NOW | RTLD_GLOBAL);
    if (!library) { std::fprintf(stderr, "%s\n", dlerror()); return 1; }
    const char *symbols[] = {
        "add_dram", "cloes_dram", "run_ns", "dram_can_accept_req",
        "dram_has_read_rsp", "dram_has_write_rsp", "dram_get_write_rsp",
        "dram_write_buffer", "dram_write_strobe", "dram_send_req",
        "dram_get_read_rsp", "dram_get_read_rsp_byte", "dram_get_inflight_read",
        "dram_preload_byte", "dram_check_byte", "dram_load_elf", "dram_load_memfile",
        "dram_register_async_callback"
    };
    for (const char *symbol : symbols) {
        dlerror();
        if (!dlsym(library, symbol)) {
            std::fprintf(stderr, "Missing DRAMSys symbol %s: %s\n", symbol, dlerror());
            return 1;
        }
    }
    Tick tick("tick");
    sc_core::sc_start();
    if (sc_core::sc_time_stamp() != sc_core::sc_time(1, sc_core::SC_NS)) return 1;
    std::puts("PASS: SystemC thread advanced 1 ns; DRAMSys loaded and exported the expected API.");
    return 0;
}
