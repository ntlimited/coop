"""Propagate layout-affecting options to coop and every C++ consumer."""

load("@bazel_skylib//rules:common_settings.bzl", "BuildSettingInfo")
load("@rules_cc//cc/common:cc_common.bzl", "cc_common")
load("@rules_cc//cc/common:cc_info.bzl", "CcInfo")

def _configuration_impl(ctx):
    stack = ctx.attr.stack[BuildSettingInfo].value
    mode = ctx.attr.mode[BuildSettingInfo].value
    level = ctx.attr.level[BuildSettingInfo].value
    if stack <= 0:
        fail("default_stack_size must be positive")
    if mode not in [0, 1, 2]:
        fail("perf_mode must be 0, 1, or 2")
    if level not in range(7):
        fail("spdlog_active_level must be between 0 (trace) and 6 (off)")
    defines = [
        "COOP_DEFAULT_STACK_SIZE=%d" % stack,
        "COOP_PERF_MODE=%d" % mode,
        "SPDLOG_ACTIVE_LEVEL=%d" % level,
    ]
    if ctx.attr.pbuf_workaround[BuildSettingInfo].value:
        defines.append("COOP_WORKAROUND_UBUNTU_PBUF_RESERVED_CHECK=1")
    for attr, macro in [(ctx.attr.counters, "COOP_PERF_USER_COUNTERS"),
                        (ctx.attr.families, "COOP_PERF_USER_FAMILIES")]:
        path = attr[BuildSettingInfo].value
        if path:
            if '"' in path or "\\" in path or "\n" in path:
                fail("perf definition paths must be plain include paths")
            defines.append('%s="%s"' % (macro, path))
    return [cc_common.merge_cc_infos(cc_infos = [
        ctx.attr.defs[CcInfo],
        CcInfo(compilation_context = cc_common.create_compilation_context(
            defines = depset(defines),
        )),
    ])]

coop_configuration = rule(
    implementation = _configuration_impl,
    attrs = {
        "stack": attr.label(default = "//bazel:default_stack_size"),
        "mode": attr.label(default = "//bazel:perf_mode"),
        "level": attr.label(default = "//bazel:spdlog_active_level"),
        "counters": attr.label(default = "//bazel:perf_user_counters"),
        "families": attr.label(default = "//bazel:perf_user_families"),
        "defs": attr.label(default = "//bazel:perf_user_defs", providers = [CcInfo]),
        "pbuf_workaround": attr.label(default = "//bazel:workaround_ubuntu_pbuf_reserved_check"),
    },
)
