"""Executed inside GDB by gdb_smoke.py, against both a live inferior and a core."""
import gdb


def run_checks():
    inferior = gdb.selected_inferior()
    thread_before = gdb.selected_thread()
    frame_before = gdb.selected_frame()
    registers_before = tuple(str(frame_before.read_register(reg)) for reg in ("sp", "pc"))
    context = gdb.parse_and_eval("g_context").dereference()
    stack_start = int(context["m_segment"]["m_bottom"].address)
    stack_size = int(context["m_segment"]["m_size"])
    stack_before = bytes(inferior.read_memory(stack_start, stack_size))
    address = int(context.address)
    trace = gdb.execute("coop bt %#x" % address, to_string=True)
    assert "gdb-parked" in trace, trace
    assert "GdbParkLeaf" in trace and "GdbParkOuter" in trace, trace
    assert "gdb_fixture.cpp:" in trace, trace
    gdb.write(trace)

    listing = gdb.execute("coop contexts", to_string=True)
    all_traces = gdb.execute("coop bt all", to_string=True)
    if not bool(gdb.parse_and_eval("g_bare")):
        assert "gdb-parked" in listing and "gdb-running" in listing, listing
        assert "BLOCKED" in listing and "RUNNING" in listing, listing
        assert "parent=%#x" % int(gdb.parse_and_eval("g_runningContext")) in listing, listing
        assert "GdbParkOuter" in all_traces, all_traces
        assert "saved registers are stale" in all_traces, all_traces
    else:
        bad = gdb.execute("coop bt %#x" % int(gdb.parse_and_eval("g_badContext")), to_string=True)
        assert "saved SP outside stack bounds or unaligned" in bad, bad

    invalid = gdb.execute("coop bt 0x8", to_string=True)
    assert "truncated" in invalid, invalid
    try:
        gdb.execute("coop bt (g_context = 0)", to_string=True)
        raise AssertionError("An expression was accepted as an address")
    except gdb.error:
        pass
    assert int(gdb.parse_and_eval("g_context")) == address
    assert bytes(inferior.read_memory(stack_start, stack_size)) == stack_before
    assert gdb.selected_thread() == thread_before and gdb.selected_frame() == frame_before
    assert tuple(str(frame_before.read_register(reg)) for reg in ("sp", "pc")) == registers_before
    gdb.write("COOP_GDB_CHECKS_PASSED\n")


try:
    run_checks()
except Exception:
    import traceback
    traceback.print_exc()
    gdb.execute("quit 1")
