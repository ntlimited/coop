"""Exercise consumer settings without changing the configuration of other tests."""

def _consumer_configuration_impl(settings, attr):
    return {
        "//bazel:default_stack_size": 262144,
        "//bazel:perf_mode": 2,
        "//bazel:spdlog_active_level": 3,
        "//bazel:perf_user_counters": "bazel/tests/counters.def",
        "//bazel:perf_user_families": "bazel/tests/families.def",
        "//bazel:perf_user_defs": "//bazel/tests:consumer_defs",
    }

_consumer_configuration = transition(
    implementation = _consumer_configuration_impl,
    inputs = [],
    outputs = [
        "//bazel:default_stack_size",
        "//bazel:perf_mode",
        "//bazel:spdlog_active_level",
        "//bazel:perf_user_counters",
        "//bazel:perf_user_families",
        "//bazel:perf_user_defs",
    ],
)

def _consumer_test_impl(ctx):
    binary = ctx.attr.binary[0][DefaultInfo]
    executable = ctx.actions.declare_file(ctx.label.name)
    ctx.actions.symlink(
        output = executable,
        target_file = binary.files_to_run.executable,
        is_executable = True,
    )
    return [DefaultInfo(
        executable = executable,
        runfiles = binary.default_runfiles.merge(binary.data_runfiles),
    )]

consumer_test = rule(
    implementation = _consumer_test_impl,
    test = True,
    attrs = {
        "binary": attr.label(
            mandatory = True,
            cfg = _consumer_configuration,
        ),
        "_allowlist_function_transition": attr.label(
            default = "@bazel_tools//tools/allowlists/function_transition_allowlist",
        ),
    },
)
