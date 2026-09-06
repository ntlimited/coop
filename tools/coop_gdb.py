"""Read-only coop context inspection. Load with: source tools/coop_gdb.py."""

import gdb

MAX_NODES = 4096
MAX_FRAMES = 128
MAX_NAME = 128
MAX_STACK = 1 << 30


def note(message):
    gdb.write("  [" + message + "]\n")


def stopped():
    inferior = gdb.selected_inferior()
    if any(thread.is_running() for thread in inferior.threads()):
        raise gdb.GdbError("Stop all inferior threads before inspecting coop.")
    if gdb.lookup_type("void").pointer().sizeof != 8:
        raise gdb.GdbError("coop inspection requires a 64-bit target.")
    return inferior


def name(value):
    # Character-at-a-time reads allow short strings at mapping boundaries. Bound both
    # pointer and fixed-array strings, and escape control characters in debugger output.
    if value.type.code == gdb.TYPE_CODE_ARRAY:
        limit = min(MAX_NAME, value.type.sizeof)
        pointer = value.address.cast(gdb.lookup_type("char").pointer())
    else:
        if not int(value):
            return "[anonymous]"
        limit, pointer = MAX_NAME, value
    result = bytearray()
    try:
        for index in range(limit):
            byte = int(pointer[index]) & 255
            if byte == 0:
                return repr(result.decode("utf-8", errors="replace"))
            result.append(byte)
    except gdb.error:
        return repr(result.decode("utf-8", errors="replace")) + " [unreadable name]"
    return repr(result.decode("utf-8", errors="replace")) + " [name truncated]"


def objects(linked_list, owner_type, budget):
    sentinel = linked_list["sentinel"]
    hook_type = sentinel.type.strip_typedefs()
    # Derive the embedded base offset from DWARF, including non-first list hooks.
    bases = [field for field in owner_type.fields()
             if field.is_base_class and field.type.strip_typedefs() == hook_type]
    if len(bases) != 1 or bases[0].bitpos is None:
        raise gdb.GdbError("Cannot locate coop list base in target debug information.")
    offset = bases[0].bitpos // 8
    end = int(sentinel.address)
    pointer = sentinel["next"]
    seen = set()
    while int(pointer) != end:
        address = int(pointer)
        if address == 0 or address % 8 or address in seen:
            note("list truncated: null, unaligned, or cyclic link at %#x" % address)
            return
        if budget[0] <= 0:
            note("list truncated: traversal limit reached (%d nodes)" % MAX_NODES)
            return
        seen.add(address)
        budget[0] -= 1
        try:
            hook = pointer.dereference()
            next_pointer = hook["next"]
            # Materialize the next address before yielding; unreadable linkage ends
            # this list but does not prevent inspecting other cooperators.
            int(next_pointer)
            yield gdb.Value(address - offset).cast(owner_type.pointer()).dereference()
            pointer = next_pointer
        except gdb.error as error:
            note("list truncated at %#x: %s" % (address, error))
            return


def contexts():
    cooperator_type = gdb.lookup_type("coop::Cooperator")
    context_type = gdb.lookup_type("coop::Context")
    registry = gdb.parse_and_eval("'coop::Cooperator::s_registry'")
    budget = [MAX_NODES]
    for cooperator in objects(registry, cooperator_type, budget):
        try:
            gdb.write("Cooperator %#x %s\n" %
                      (int(cooperator.address), name(cooperator["m_name"])))
            yield from objects(cooperator["m_contexts"], context_type, budget)
        except gdb.error as error:
            note("unreadable cooperator: %s" % error)


def describe(context):
    gdb.write("Context %#x %s state=%s parent=%#x cooperator=%#x\n" % (
        int(context.address), name(context["m_name"]), str(context["m_state"]).split("::")[-1],
        int(context["m_parent"]), int(context["m_cooperator"])))


def symbol(index, pc):
    # Saved PCs are return addresses. Attribute their call site to the previous byte,
    # rather than incorrectly naming the next function at an end-of-function boundary.
    address = max(0, pc - 1)
    label = gdb.execute("info symbol %#x" % address, to_string=True).strip()
    sal = gdb.find_pc_line(address)
    location = ""
    if sal.symtab and sal.line:
        location = " at %s:%d" % (sal.symtab.filename, sal.line)
    gdb.write("  #%d %#x %s%s\n" % (index, pc, label, location))
    return label.startswith("CoopContextEntry ")


def backtrace(context):
    describe(context)
    state = str(context["m_state"]).split("::")[-1]
    scheduled = False
    owner = context["m_cooperator"]
    if int(owner):
        try:
            scheduled = int(owner.dereference()["m_scheduled"]) == int(context.address)
        except gdb.error:
            note("cooperator unreadable; cannot cross-check scheduled context")
    if state == "RUNNING" or scheduled:
        note("running context: saved registers are stale; use thread apply all bt for live stacks")
        return
    if state != "YIELDED" and state != "BLOCKED":
        note("context has no suspended stack in this state")
        return
    inferior = gdb.selected_inferior()
    architecture = inferior.architecture().name()
    if architecture.startswith("i386:x86-64"):
        fp_offset, pc_offset, saved_size = 40, 48, 56
    elif architecture.startswith("aarch64"):
        fp_offset, pc_offset, saved_size = 0, 8, 96
    else:
        raise gdb.GdbError("Unsupported coop saved-register architecture: " + architecture)
    segment = context["m_segment"]
    bottom = int(segment["m_bottom"].address)
    size = int(segment["m_size"])
    top = bottom + size
    if size <= 0 or size > MAX_STACK or top >= 1 << 64:
        note("stack truncated: invalid segment bounds")
        return
    # The bump heap shares the segment. Treat its current watermark as the lower
    # bound when available, so a bogus frame does not walk through user allocations.
    heap_top = int(context["m_heapTop"])
    if heap_top:
        if not bottom <= heap_top <= top:
            note("stack truncated: invalid heap watermark")
            return
        bottom = heap_top
    sp = int(context["m_sp"])
    if sp % 8 or not bottom <= sp <= top - saved_size:
        note("stack truncated: saved SP outside stack bounds or unaligned")
        return
    word_type = gdb.lookup_type("unsigned long long").pointer()

    def word(address):
        return int(gdb.Value(address).cast(word_type).dereference())

    fp, pc = word(sp + fp_offset), word(sp + pc_offset)
    if not pc:
        note("stack truncated: null saved return address")
        return
    if symbol(0, pc):
        return
    floor = sp + saved_size
    for index in range(1, MAX_FRAMES):
        if fp == 0:
            return
        if fp % 8 or not floor <= fp <= top - 16:
            note("stack truncated: frame pointer outside bounds, non-monotonic, or unaligned")
            return
        next_fp, pc = word(fp), word(fp + 8)
        if not pc:
            return
        if symbol(index, pc):
            return
        floor, fp = fp + 16, next_fp
    note("stack truncated: frame limit reached (%d)" % MAX_FRAMES)


class Coop(gdb.Command):
    """Inspect coop without executing inferior functions or changing inferior state."""

    def __init__(self):
        super().__init__("coop", gdb.COMMAND_STACK, prefix=True)


class Contexts(gdb.Command):
    """coop contexts: list contexts, states, names, parents, and cooperator addresses."""

    def __init__(self):
        super().__init__("coop contexts", gdb.COMMAND_STACK)

    def invoke(self, argument, from_tty):
        self.dont_repeat()
        if argument.strip():
            raise gdb.GdbError("Usage: coop contexts")
        stopped()
        for context in contexts():
            try:
                describe(context)
            except gdb.error as error:
                note("unreadable context: %s" % error)


class Backtrace(gdb.Command):
    """coop bt ADDRESS|all: bounded saved-stack trace (no locals or DWARF unwinding)."""

    def __init__(self):
        super().__init__("coop bt", gdb.COMMAND_STACK)

    def invoke(self, argument, from_tty):
        self.dont_repeat()
        stopped()
        argument = argument.strip()
        if argument == "all":
            values = contexts()
        else:
            # Only accept addresses. In particular, never evaluate user expressions
            # that could call an inferior function or assign to inferior memory.
            try:
                address = int(argument, 0)
            except ValueError:
                raise gdb.GdbError("Usage: coop bt <numeric context-address>|all")
            if not 0 < address < 1 << 64 or address % 8:
                raise gdb.GdbError("Expected a nonzero aligned 64-bit context address.")
            values = [gdb.Value(address).cast(gdb.lookup_type("coop::Context").pointer())
                      .dereference()]
        for context in values:
            try:
                backtrace(context)
            except gdb.error as error:
                note("stack truncated: %s" % error)


Coop()
Contexts()
Backtrace()
