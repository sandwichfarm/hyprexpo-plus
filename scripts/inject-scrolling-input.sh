#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

declare -A cases expectations

cases[hover-clear]='mouse_move:410:18|mouse_move:-1:0'
cases[non-primary-passthrough]='mouse_button:410:18:272:1'
cases[outside-primary-passthrough]='mouse_button:-1:18:273:1'
cases[mouse-click]='mouse_button:410:18:273:1|mouse_move:421:18|mouse_button:421:18:273:0'
cases[threshold-drag]='mouse_button:410:18:273:1|mouse_move:421:18|mouse_move:422:18'
cases[same-column]='mouse_button:410:18:273:1|mouse_move:422:18|mouse_move:410:120|mouse_button:410:120:273:0'
cases[new-column-before]='mouse_button:410:18:273:1|mouse_move:422:18|mouse_move:351:18|mouse_button:351:18:273:0'
cases[new-column-after]='mouse_button:410:18:273:1|mouse_move:422:18|mouse_move:549:18|mouse_button:549:18:273:0'
cases[cross-scrolling]='mouse_button:410:18:273:1|mouse_move:422:18|mouse_move:500:180|mouse_button:500:180:273:0'
cases[mixed-workspace]='mouse_button:410:18:273:1|mouse_move:422:18|mouse_move:500:340|mouse_button:500:340:273:0'
cases[terminal-workspace]='mouse_button:410:18:273:1|mouse_move:422:18|mouse_move:500:490|mouse_button:500:490:273:0'
cases[outside-release]='mouse_button:410:18:273:1|mouse_move:422:18|mouse_move:-1:0|mouse_button:-1:0:273:0'
cases[no-op-release]='mouse_button:410:18:273:1|mouse_move:422:18|mouse_button:422:18:273:0'
cases[axis-inside]='mouse_axis:410:18:60'
cases[axis-outside]='mouse_axis:-1:18:60'
cases[axis-clamped]='mouse_axis:410:18:999'
cases[axis-owned]='mouse_button:410:18:273:1|mouse_axis:410:18:60|reset:cancel'
cases[mouse-canvas-pan]='mouse_button:900:155:273:1|mouse_move:900:105|mouse_button:900:105:273:0'
cases[touch-pan]='touch_down:1:900:155|touch_motion:1:900:105|touch_up:1'
cases[touch-tap]='touch_down:1:410:18|touch_motion:1:421:18|touch_up:1'
cases[touch-same-column]='touch_down:1:410:18|touch_motion:1:422:18|touch_motion:1:410:120|touch_up:1'
cases[touch-new-column-before]='touch_down:1:410:18|touch_motion:1:422:18|touch_motion:1:351:18|touch_up:1'
cases[touch-new-column-after]='touch_down:1:410:18|touch_motion:1:422:18|touch_motion:1:549:18|touch_up:1'
cases[touch-cross-scrolling]='touch_down:1:410:18|touch_motion:1:422:18|touch_motion:1:500:180|touch_up:1'
cases[touch-mixed-workspace]='touch_down:1:410:18|touch_motion:1:422:18|touch_motion:1:500:340|touch_up:1'
cases[touch-terminal-workspace]='touch_down:1:410:18|touch_motion:1:422:18|touch_motion:1:500:490|touch_up:1'
cases[touch-outside-release]='touch_down:1:410:18|touch_motion:1:422:18|touch_motion:1:-1:0|touch_up:1'
cases[touch-no-op-release]='touch_down:1:410:18|touch_motion:1:422:18|touch_up:1'
cases[touch-cancel-pending]='touch_down:1:410:18|touch_cancel:1'
cases[touch-cancel-pan]='touch_down:1:900:155|touch_motion:1:900:105|touch_cancel:1'
cases[touch-cancel-drag]='touch_down:1:410:18|touch_motion:1:422:18|touch_cancel:1'
cases[mismatched-cancel]='touch_down:1:410:18|touch_cancel:2|touch_cancel:1'
cases[touch-reacquire]='touch_down:1:410:18|touch_cancel:1|touch_down:2:410:18'
cases[mouse-reacquire]='touch_down:1:410:18|touch_cancel:1|mouse_button:410:18:273:1'
cases[touch-axis-owned]='touch_down:1:410:18|mouse_axis:410:18:60|touch_cancel:1'
cases[touch-pan-axis-owned]='touch_down:1:900:155|touch_motion:1:900:105|mouse_axis:900:105:60|touch_up:1'
cases[stale-id]='touch_down:1:410:18|reset:refresh|touch_motion:1:422:18'
cases[refresh-reset]='mouse_button:410:18:273:1|reset:refresh'
cases[teardown-reset]='touch_down:1:900:155|reset:teardown|reset:teardown'

idle='Idle,false,false,false,0,0,false,0,0,false,false,false,false,false,None'
mouse_press='MousePressPending,true,false,false,0,0,false,0,0,false,false,false,false,false,None'
touch_press='TouchPressPending,true,false,false,0,0,false,0,0,false,false,false,false,false,None'
mouse_under='MousePressPending,true,false,false,0,0,false,0,0,false,false,false,false,false,None'
mouse_begin='WindowDrag,true,false,false,0,0,false,0,0,true,false,false,false,false,NoOp'
touch_begin='WindowDrag,true,false,false,0,0,false,0,0,true,false,false,false,false,NoOp'

drag_case() {
    local name=$1 prefix=$2 drop=$3
    local press=$mouse_press begin=$mouse_begin
    if [[ $prefix == touch ]]; then press=$touch_press; begin=$touch_begin; fi
    expectations[$name]="Idle,-1,true|$press;$begin;WindowDrag,true,false,false,0,0,false,0,0,false,true,false,false,false,$drop;Idle,true,false,false,0,0,false,0,0,false,false,true,false,true,$drop"
}

expectations[hover-clear]="Idle,-1,false|Idle,false,true,false,0,0,false,0,0,false,false,false,false,false,None;Idle,false,true,true,0,0,false,0,0,false,false,false,false,false,None"
expectations[non-primary-passthrough]="Idle,-1,false|$idle"
expectations[outside-primary-passthrough]="Idle,-1,false|$idle"
expectations[mouse-click]="Idle,-1,false|$mouse_press;$mouse_under;Idle,true,false,false,0,0,true,1,201,false,false,false,false,true,None"
expectations[threshold-drag]="WindowDrag,-1,true|$mouse_press;$mouse_under;$mouse_begin"
drag_case same-column mouse ExistingColumn
drag_case new-column-before mouse NewColumnBefore
drag_case new-column-after mouse NewColumnAfter
drag_case cross-scrolling mouse CrossWorkspace
drag_case mixed-workspace mouse MixedFallback
drag_case terminal-workspace mouse TerminalWorkspace
expectations[outside-release]="Idle,-1,false|$mouse_press;$mouse_begin;WindowDrag,true,false,false,0,0,false,0,0,false,true,false,false,false,None;Idle,true,false,false,0,0,false,0,0,false,false,false,true,true,None"
expectations[no-op-release]="Idle,-1,true|$mouse_press;$mouse_begin;Idle,true,false,false,0,0,false,0,0,false,false,true,false,true,NoOp"
expectations[axis-inside]="Idle,-1,false|Idle,true,false,false,60,60,false,0,0,false,false,false,false,false,None"
expectations[axis-outside]="Idle,-1,false|$idle"
expectations[axis-clamped]="Idle,-1,false|Idle,true,false,false,130,130,false,0,0,false,false,false,false,false,None"
expectations[axis-owned]="Idle,-1,false|$mouse_press;$mouse_press;Idle,false,false,true,0,0,false,0,0,false,false,false,true,true,None"
expectations[mouse-canvas-pan]="Idle,-1,false|CanvasPan,true,false,false,0,0,false,0,0,false,false,false,false,false,None;CanvasPan,true,false,false,50,50,false,0,0,false,false,false,false,false,None;Idle,true,false,false,50,0,false,0,0,false,false,false,false,true,None"
expectations[touch-pan]="Idle,-1,false|CanvasPan,true,false,false,0,0,false,0,0,false,false,false,false,false,None;CanvasPan,true,false,false,50,50,false,0,0,false,false,false,false,false,None;Idle,true,false,false,50,0,false,0,0,false,false,false,false,true,None"
expectations[touch-tap]="Idle,-1,false|$touch_press;TouchPressPending,true,false,false,0,0,false,0,0,false,false,false,false,false,None;Idle,true,false,false,0,0,true,1,201,false,false,false,false,true,None"
drag_case touch-same-column touch ExistingColumn
drag_case touch-new-column-before touch NewColumnBefore
drag_case touch-new-column-after touch NewColumnAfter
drag_case touch-cross-scrolling touch CrossWorkspace
drag_case touch-mixed-workspace touch MixedFallback
drag_case touch-terminal-workspace touch TerminalWorkspace
expectations[touch-outside-release]="Idle,-1,false|$touch_press;$touch_begin;WindowDrag,true,false,false,0,0,false,0,0,false,true,false,false,false,None;Idle,true,false,false,0,0,false,0,0,false,false,false,true,true,None"
expectations[touch-no-op-release]="Idle,-1,true|$touch_press;$touch_begin;Idle,true,false,false,0,0,false,0,0,false,false,true,false,true,NoOp"
expectations[touch-cancel-pending]="Idle,-1,false|$touch_press;Idle,true,false,true,0,0,false,0,0,false,false,false,true,true,None"
expectations[touch-cancel-pan]="Idle,-1,false|CanvasPan,true,false,false,0,0,false,0,0,false,false,false,false,false,None;CanvasPan,true,false,false,50,50,false,0,0,false,false,false,false,false,None;Idle,true,false,false,50,0,false,0,0,false,false,false,false,true,None"
expectations[touch-cancel-drag]="Idle,-1,false|$touch_press;$touch_begin;Idle,true,false,true,0,0,false,0,0,false,false,false,true,true,None"
expectations[mismatched-cancel]="Idle,-1,false|$touch_press;TouchPressPending,false,false,false,0,0,false,0,0,false,false,false,false,false,None;Idle,true,false,true,0,0,false,0,0,false,false,false,true,true,None"
expectations[touch-reacquire]="TouchPressPending,2,false|$touch_press;Idle,true,false,true,0,0,false,0,0,false,false,false,true,true,None;TouchPressPending,true,false,false,0,0,false,0,0,false,false,false,false,false,None"
expectations[mouse-reacquire]="MousePressPending,-1,false|$touch_press;Idle,true,false,true,0,0,false,0,0,false,false,false,true,true,None;$mouse_press"
expectations[touch-axis-owned]="Idle,-1,false|$touch_press;TouchPressPending,true,false,false,0,0,false,0,0,false,false,false,false,false,None;Idle,true,false,true,0,0,false,0,0,false,false,false,true,true,None"
expectations[touch-pan-axis-owned]="Idle,-1,false|CanvasPan,true,false,false,0,0,false,0,0,false,false,false,false,false,None;CanvasPan,true,false,false,50,50,false,0,0,false,false,false,false,false,None;CanvasPan,true,false,false,50,0,false,0,0,false,false,false,false,false,None;Idle,true,false,false,50,0,false,0,0,false,false,false,false,true,None"
expectations[stale-id]="Idle,-1,false|$touch_press;Idle,false,false,true,0,0,false,0,0,false,false,false,true,true,None;$idle"
expectations[refresh-reset]="Idle,-1,false|$mouse_press;Idle,false,false,true,0,0,false,0,0,false,false,false,true,true,None"
expectations[teardown-reset]="Idle,-1,false|CanvasPan,true,false,false,0,0,false,0,0,false,false,false,false,false,None;Idle,false,false,false,0,0,false,0,0,false,false,false,false,true,None;Idle,false,false,false,0,0,false,0,0,false,false,false,false,true,None"

assert_event() {
    local json=$1 index=$2 encoded=$3
    local state consume hover clear pan pan_delta select select_ws select_token begin update finish cancel reset drop
    IFS=, read -r state consume hover clear pan pan_delta select select_ws select_token begin update finish cancel reset drop <<<"$encoded"
    jq -e --argjson i "$index" --arg state "$state" --argjson consume "$consume" --argjson hover "$hover" --argjson clear "$clear" \
        --argjson pan "$pan" --argjson panDelta "$pan_delta" --argjson select "$select" --argjson selectWorkspaceId "$select_ws" --argjson selectTargetToken "$select_token" \
        --argjson beginDrag "$begin" --argjson updateDrag "$update" --argjson finishDrag "$finish" --argjson cancelDrag "$cancel" --argjson resetOwnership "$reset" --arg drop "$drop" \
        '.events[$i] | .state == $state and .consume == $consume and .hoverChanged == $hover and .clearHover == $clear and .pan == $pan and .panDelta == $panDelta and
         .select == $select and .selectWorkspaceId == $selectWorkspaceId and .selectTargetToken == $selectTargetToken and .beginDrag == $beginDrag and
         .updateDrag == $updateDrag and .finishDrag == $finishDrag and .cancelDrag == $cancelDrag and .resetOwnership == $resetOwnership and .drop == $drop' \
        <<<"$json" >/dev/null
}

assert_case() {
    local name=$1 json=$2
    local expected=${expectations[$name]}
    local final records final_state owning_touch has_drop
    final=${expected%%|*}; records=${expected#*|}
    IFS=, read -r final_state owning_touch has_drop <<<"$final"
    jq -e --arg requestId "$name-requestId" --arg finalState "$final_state" --argjson owningTouchId "$owning_touch" --argjson hasDropIntent "$has_drop" \
        '.requestId == $requestId and .finalState == $finalState and .owningTouchId == $owningTouchId and .hasDropIntent == $hasDropIntent' <<<"$json" >/dev/null
    local index=0 record
    IFS=';' read -ra expected_records <<<"$records"
    jq -e --argjson count "${#expected_records[@]}" '.events | length == $count' <<<"$json" >/dev/null
    for record in "${expected_records[@]}"; do
        assert_event "$json" "$index" "$record"
        index=$((index + 1))
    done
}

run_oracle_case() {
    local name=$1
    local request_id="$name-requestId"
    local json
    json=$("$repo_root/ScrollingInputOracle" "$request_id|${cases[$name]}")
    assert_case "$name" "$json"
}

source_contract() {
    cd "$repo_root"
    local token name
    for token in 'm_events.input.mouse.move.listen' 'm_events.input.mouse.button.listen' 'm_events.input.mouse.axis.listen' 'm_events.input.touch.down.listen' \
        'm_events.input.touch.motion.listen' 'm_events.input.touch.up.listen' 'm_events.input.touch.cancel.listen' 'info.cancelled = info.cancelled || effects.consume' 'transitionInput(' 'touchToGlobalLogical('; do
        rg -Fq "$token" src/ScrollingOverview.cpp
    done
    rg -Fq 'plugin:hyprexpo:scrolling_input_debug' src/PluginConfig.cpp src/Dispatchers.cpp
    rg -Fq 'hyprexpo:scrolling_input_test' src/Dispatchers.cpp
    rg -Fq 'HYPREXPO_SCROLLING_INPUT {}' src/Dispatchers.cpp
    make test ScrollingInputOracle
    for name in "${!cases[@]}"; do
        printf 'oracle %s\n' "$name"
        run_oracle_case "$name"
    done
    # Runtime entry has no dynamically scoped loop variable named "name".
    (unset name; run_oracle_case touch-cancel-pending)
    printf 'scrolling input behavioral oracle PASS (%s cases)\n' "${#cases[@]}"
}

if [[ ${1:-} == --source-contract ]]; then source_contract; exit 0; fi

if [[ -z ${HYPRLAND_INSTANCE_SIGNATURE:-} ]]; then
    printf '%s\n' 'HYPRLAND_INSTANCE_SIGNATURE is required for runtime injection' >&2
    exit 1
fi
case_name=${1:-}
if [[ -z $case_name || -z ${cases[$case_name]+x} ]]; then
    printf 'usage: %s <%s>\n' "$0" "$(IFS='|'; printf '%s' "${!cases[*]}")" >&2
    exit 1
fi
debug_value=$(hyprctl getoption plugin:hyprexpo:scrolling_input_debug -j | jq -r '.int // 0')
[[ $debug_value == 1 ]] || { printf '%s\n' 'plugin:hyprexpo:scrolling_input_debug must be 1' >&2; exit 1; }
request_id="$case_name-requestId"
log_file="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/hypr/$HYPRLAND_INSTANCE_SIGNATURE/hyprland.log"
hyprctl eval "hl.plugin.hyprexpo.scrolling_input_test(\"$request_id|${cases[$case_name]}\")" >/dev/null
for _ in {1..50}; do
    record=$(rg -F "HYPREXPO_SCROLLING_INPUT {\"requestId\":\"$request_id\"" "$log_file" | tail -1 || true)
    if [[ -n $record ]]; then
        json=${record#*HYPREXPO_SCROLLING_INPUT }
        assert_case "$case_name" "$json"
        printf '%s\n' "$json"
        exit 0
    fi
    sleep 0.02
done
printf 'no request-correlated input diagnostic found for %s\n' "$request_id" >&2
exit 1
