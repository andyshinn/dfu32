# SPDX-License-Identifier: MIT
"""The bench checklist (docs/BENCH.md §7), one function per item.

Every check gets the device into the state it needs through the ctx.ensure_*
helpers, so any of them can run on its own with --only.
"""
from __future__ import annotations

import csv
import platform
import statistics
import subprocess
import threading
import time
from pathlib import Path
from typing import Optional

from .parse import parse_check, parse_stage, parse_token
from .runner import TERMINAL, Ctx, TestFailed, TestSkipped, check

WRONG_BOARD = "not_this_board"


def wait_state(ctx: Ctx, job_id: int, states: set, timeout: float, *, poll: float = 0.5) -> dict:
    """Polls /status until job `job_id` reaches one of `states` (or ends)."""
    end = time.monotonic() + timeout
    last = None
    while time.monotonic() < end:
        st = ctx.api.status(timeout=3) if ctx.api else None
        job = (st or {}).get("job") or {}
        if job.get("id") == job_id:
            if job.get("state") != last:
                last = job.get("state")
                ctx.say(f"job #{job_id}: {last}{' / ' + job['phase'] if job.get('phase') else ''}")
            if job.get("state") in states or job.get("state") in TERMINAL:
                return job
        time.sleep(poll)
    raise TestFailed(f"job #{job_id} never reached {'/'.join(sorted(states))} within {timeout:.0f} s (last {last})")


def final_job(ctx: Ctx, w) -> dict:
    """The job as /status reports it once the API is back (console snapshots have fewer fields)."""
    st = ctx.wait_api(120)
    job = st.get("job") or {}
    return job if job.get("id") == w.job_id else w.final


def stage_cmd(ctx: Ctx, args: str = "") -> dict:
    out = ctx.con.cmd(f"stage {args}".strip(), 240)
    r = parse_stage(out)
    ctx.say(f"stage {args}: {'OK' if r['ok'] else r['error']}{' (reused)' if r['reused'] else ''}")
    return r


# ============================================================================================
# 1. Leftovers from M2b (console, no WiFi needed)
# ============================================================================================


@check("1.1", "runs / run N show a console flash", needs=("repeater",))
def c_runs_after_flash(ctx: Ctx) -> None:
    ctx.ensure_target_console(url=False)
    ctx.ensure_staged()
    res = ctx.console_flash()
    ctx.expect(res["ok"], "console `flash --yes` succeeded", res["error"])
    seq = res["run"]
    ctx.expect(seq > 0, f"the flash was recorded as run #{seq}")
    row = next((r for r in ctx.runs() if r["seq"] == seq), None)
    ctx.expect(row is not None, f"`runs` lists run #{seq}")
    ctx.expect(row["result"] == "SUCCEEDED", f"`runs` shows run #{seq} SUCCEEDED", row["result"])
    rec, log_bytes = ctx.run_record(seq)
    ctx.expect(rec is not None and rec.get("seq") == seq, f"`run {seq}` prints the JSON record")
    ctx.expect(log_bytes > 0, f"`run {seq}` includes a log tail ({log_bytes} bytes)")
    ctx.data("run", rec)


@check("1.2", "flash --interval 8: overrun, retry, both attempts recorded", needs=("repeater",))
def c_retry_interval_8(ctx: Ctx) -> None:
    ctx.ensure_target_console(url=False)
    ctx.ensure_staged()
    res = ctx.console_flash("--interval 8")
    rec = ctx.run_record(res["run"])[0] if res["run"] else None
    atts = (rec or {}).get("attempts", [])
    ctx.data("attempts", atts)
    if not res["ok"]:
        if res["erased"]:
            ctx.recover_target()
        raise TestFailed(f"all attempts failed: {res['error']}")
    ctx.expect(rec is not None, f"run #{res['run']} recorded")
    for a in atts:
        ctx.note(f"attempt {a.get('n')}: interval {a.get('interval_ms')} ms, PRN {a.get('prn')}, "
                 f"{a.get('kind')} / {a.get('outcome')}, {a.get('bytes_acked')} B acked")
    if len(atts) < 2:
        ctx.warn("8 ms didn't overrun the bootloader this time: one attempt, so the retry path wasn't exercised")
        return
    ctx.expect(atts[0].get("kind") not in ("", "NONE"), f"attempt 1 failed and was classified {atts[0].get('kind')}")
    ctx.soft(atts[0].get("kind") == "LINK_OR_OVERRUN", "attempt 1 classified LINK_OR_OVERRUN", atts[0].get("kind"))
    ctx.soft(atts[1].get("interval_ms", 0) >= 30, f"attempt 2 backed off to {atts[1].get('interval_ms')} ms")
    ctx.expect(atts[-1].get("outcome") == "OK", "the last attempt succeeded")
    ctx.expect(rec.get("result") == "SUCCEEDED", "the run record says SUCCEEDED")


@check("1.3", "reset mid-transfer -> ABORTED_BY_REBOOT, then flash recovers", needs=("repeater",))
def c_reboot_mid_flash(ctx: Ctx) -> None:
    ctx.ensure_target_console(url=False)
    ctx.ensure_staged()
    box: dict = {}

    def interrupt(mark: int) -> bool:
        since = mark
        while True:
            hit = ctx.con.wait_line(r"progress\s+(\d+)%", ctx.args.start_ota_timeout + 120, since=since)
            if hit is None:
                raise TestFailed("the transfer never started (no progress lines)")
            if int(hit[2].group(1)) >= 10:
                break
            since = hit[0] + 1
        ctx.say(f"transfer at {hit[2].group(1)}%: resetting DFU32")
        box["boot"] = ctx.reset_and_wait(why="mid-transfer")
        return True

    ctx.console_flash(until=interrupt)
    text = ctx.con.boot_lines(box["boot"])
    ctx.expect("interrupted by a reboot" in text, "the next boot logs the interrupted flash")
    rows = ctx.runs()
    ctx.expect(rows and rows[0]["result"] == "ABORTED_BY_REBOOT", "newest run is ABORTED_BY_REBOOT",
               rows[0]["result"] if rows else "no runs")
    rec, _ = ctx.run_record(rows[0]["seq"])
    ctx.expect(rec and rec.get("target_erased") == "possibly", "the record says target_erased: possibly",
               (rec or {}).get("target_erased"))
    res = ctx.console_flash(start_ota=False, wait=180)
    ctx.expect(res["ok"], "`flash --yes` recovered the repeater (no `start ota` needed)", res["error"])


@check("1.4", "run records survive a reboot")
def c_records_survive(ctx: Ctx) -> None:
    before = [r["seq"] for r in ctx.runs()]
    if not before:
        raise TestSkipped("no run records yet (run 1.1 first)")
    if ctx.args.power_cycle:
        boots = len(ctx.con.boots)
        ctx.pause("Unplug DFU32's USB cable, wait 5 s, and plug it back in.")
        ctx.after_boot(ctx.con.wait_boot(boots, 120))
    else:
        ctx.reset_and_wait()
        ctx.note("used a hard reset (EN pin) instead of a power cycle; --power-cycle asks you to unplug it")
    after = [r["seq"] for r in ctx.runs()]
    missing = [s for s in before if s not in after]
    ctx.expect(not missing, f"all {len(before)} run records still listed", f"missing {missing}")
    ctx.expect(after[:1] == before[:1], "no new record appeared (a clean reboot isn't an aborted run)")


# ============================================================================================
# 2. M3a: fetch and stage from the console (BENCH §4)
# ============================================================================================


def _ping(host: str) -> bool:
    flag = "-n" if platform.system() == "Windows" else "-c"
    try:
        return subprocess.run(["ping", flag, "1", host], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                              timeout=10).returncode == 0
    except (OSError, subprocess.TimeoutExpired):
        return False


@check("2.1", "wifi: join, IP / RSSI / hostname, mDNS")
def c_wifi(ctx: Ctx) -> None:
    if ctx.args.wifi_ssid:
        st = ctx.join_wifi()
        ctx.expect(st["connected"], f"`wifi {ctx.args.wifi_ssid} …` connected")
    st = ctx.ensure_wifi()
    ctx.expect(bool(st["ip"]), f"`wifi` shows IP {st['ip']}")
    ctx.expect(st["rssi"] is not None, f"`wifi` shows RSSI {st['rssi']} dBm")
    ctx.expect(bool(st["hostname"]), f"`wifi` shows hostname {st['hostname']}")
    name = st["hostname"] + ".local"
    ip = ctx.resolve(name)
    ctx.soft(ip == st["ip"], f"{name} resolves to {st['ip']}", f"got {ip}; is mDNS available on this computer?")
    ctx.soft(_ping(name), f"ping {name} answers")
    end = time.monotonic() + 30  # SNTP can take a few seconds after the first connect
    while not st["time_synced"] and time.monotonic() < end:
        time.sleep(3)
        st = ctx.wifi_status()
    ctx.soft(st["time_synced"], "clock synced over SNTP")


@check("2.2", "target board/url, then check")
def c_check(ctx: Ctx) -> None:
    ctx.ensure_wifi()
    ctx.ensure_target_console()
    n0 = ctx.site.count("good")
    c = parse_check(ctx.con.cmd("check", 60))
    ctx.expect(c["ok"], "`check` fetched the manifest", c.get("error", ""))
    ctx.expect(c["version"] == ctx.site.version, f"version {c['version']}", f"expected {ctx.site.version}")
    ctx.expect(c["board_match"], f"board {ctx.args.board} matches the manifest")
    ctx.ok(f"staged: {c['staged']}")
    ctx.expect(ctx.site.count("good") == n0, "`check` didn't download the firmware")


@check("2.3", "stage: fresh download, reuse, --force")
def c_stage(ctx: Ctx) -> None:
    ctx.ensure_wifi()
    ctx.ensure_target_console()
    try:
        r = ctx.ensure_token().delete("/staging")
        ctx.expect(r.status == 204, "cleared staging first (DELETE /staging) so the first stage is fresh", r.short())
    except TestSkipped as e:
        ctx.warn(f"couldn't clear staging first ({e}); the first stage may reuse")
    n0 = ctx.site.count("good")
    s = stage_cmd(ctx)
    ctx.expect(s["ok"] and not s["reused"], f"`stage` downloaded and verified {s['bytes']} B", s["error"])
    ctx.expect(ctx.site.count("good") == n0 + 1, "firmware.bin was fetched once")
    s = stage_cmd(ctx)
    ctx.expect(s["ok"] and s["reused"], "second `stage` reused the staged image", s["error"])
    ctx.expect(ctx.site.count("good") == n0 + 1, "no second download")
    s = stage_cmd(ctx, "--force")
    ctx.expect(s["ok"] and not s["reused"], "`stage --force` downloaded again", s["error"])
    ctx.expect(ctx.site.count("good") == n0 + 2, "firmware.bin was fetched again")
    ctx.expect(ctx.staged_matches(), "`staged` verifies the image")


@check("2.4", "console flash: WiFi on (default), then --wifi off", needs=("repeater",))
def c_console_flash_wifi(ctx: Ctx) -> None:
    ctx.ensure_wifi()
    ctx.ensure_target_console()
    ctx.ensure_staged()
    a = ctx.console_flash()
    ctx.expect(a["ok"], "flash with the default (WiFi on) succeeded", a["error"])
    ctx.expect(not a["wifi_off"], "WiFi stayed up by default")
    b = ctx.console_flash("--wifi off")
    ctx.expect(b["ok"], "flash with --wifi off succeeded", b["error"])
    ctx.expect(b["wifi_off"], "--wifi off stopped WiFi for the transfer")
    ctx.expect(b["wifi_restored"], "WiFi was restored afterwards")
    ctx.expect(ctx.ensure_wifi(wait=30)["connected"], "`wifi` is connected again")
    ra = ctx.run_record(a["run"])[0] or {}
    rb = ctx.run_record(b["run"])[0] or {}
    ctx.expect(ra.get("wifi_off") is False and rb.get("wifi_off") is True, "run records carry wifi_off false / true")
    ctx.note(f"throughput: WiFi on {a['throughput_kBps']} kB/s, WiFi off {b['throughput_kBps']} kB/s")


def _stage_fault(ctx: Ctx, variant: str, code: str, *, keeps_old: bool, extra: str = "") -> None:
    ctx.ensure_wifi()
    ctx.ensure_target_console()
    ctx.ensure_staged()
    try:
        ctx.con.cmd(f"target url {ctx.manifest_url(variant)}", 10)
        s = stage_cmd(ctx, "--force")
        ctx.expect(not s["ok"] and s["error"].startswith(code), f"stage failed with {code}", s["error"])
        if extra:
            ctx.expect(extra in s["error"], f"the error mentions '{extra}'", s["error"])
    finally:
        ctx.con.cmd(f"target url {ctx.manifest_url()}", 10)
    if keeps_old:
        ctx.expect(ctx.staged_matches(), "the previously staged image is untouched and verifies")


@check("2.5a", "stage, wrong board -> BOARD_MISMATCH, old image kept")
def c_wrong_board(ctx: Ctx) -> None:
    ctx.ensure_wifi()
    ctx.ensure_target_console()
    ctx.ensure_staged()
    try:
        ctx.con.cmd(f"target board {WRONG_BOARD}", 10)
        s = stage_cmd(ctx)
        ctx.expect(not s["ok"] and s["error"].startswith("BOARD_MISMATCH"), "stage refused with BOARD_MISMATCH",
                   s["error"])
    finally:
        ctx.con.cmd(f"target board {ctx.args.board}", 10)
    ctx.expect(ctx.staged_matches(), "the previously staged image is untouched and verifies")


@check("2.5b", "stage, .bin one byte long -> SIZE_MISMATCH before erasing")
def c_size_mismatch(ctx: Ctx) -> None:
    _stage_fault(ctx, "badsize", "SIZE_MISMATCH", keeps_old=True)


@check("2.5c", "stage, manifest 404 -> HTTP_ERROR")
def c_http_404(ctx: Ctx) -> None:
    _stage_fault(ctx, "missing", "HTTP_ERROR", keeps_old=True, extra="404")


@check("2.5d", "stage, wrong sha256 -> HASH_MISMATCH")
def c_hash_mismatch(ctx: Ctx) -> None:
    _stage_fault(ctx, "badhash", "HASH_MISMATCH", keeps_old=False)
    s = stage_cmd(ctx)
    ctx.expect(s["ok"], "re-staged the good image afterwards", s["error"])


# ============================================================================================
# 3. M3b: the HTTP API (BENCH §6)
# ============================================================================================


@check("3.1", "claim, token auth, 429 throttle, token rotate")
def c_claim(ctx: Ctx) -> None:
    api = ctx.ensure_api()
    out = ctx.con.cmd("token clear", 10)
    ctx.expect("Unclaimed" in out, "`token clear` unclaimed the device", out)
    ctx.token = None
    api.token = None
    st = api.status()
    ctx.expect(st is not None and st["device"]["claimed"] is False, "GET /status works without a token, claimed:false")
    r = api.get("/targets", token=None)
    ctx.expect(r.status == 403 and r.error == "UNCLAIMED", "while unclaimed, other calls get 403 UNCLAIMED", r.short())
    r = api.post("/claim", token=None)
    ctx.expect(r.status == 200 and bool(r.get("token")), "POST /claim returned a token", r.short())
    ctx.set_token(r.get("token"))
    r = api.post("/claim", token=None)
    ctx.expect(r.status == 409 and r.error == "ALREADY_CLAIMED", "a second claim gets 409", r.short())
    r = api.get("/targets", token=None)
    ctx.expect(r.status == 401, "no token -> 401", r.short())
    r = api.get("/targets", token="not-the-token")
    ctx.expect(r.status == 401, "wrong token -> 401", r.short())
    statuses = [api.get("/targets", token=f"wrong-{i}", retry_429=False).status for i in range(6)]
    ctx.data("statuses", statuses)
    ctx.expect(429 in statuses, f"repeated wrong tokens are throttled with 429 ({statuses})")
    time.sleep(1.2)
    r = api.get("/targets")
    ctx.expect(r.status == 200, "the right token still works during the penalty (one check per second)", r.short())
    new = parse_token(ctx.con.cmd("token rotate", 10))
    ctx.expect(bool(new), "`token rotate` printed a new token")
    old = ctx.token
    ctx.set_token(new)
    time.sleep(1.2)
    r = api.get("/targets", token=old)
    ctx.expect(r.status == 401, "the old token stops working", r.short())
    time.sleep(1.2)
    r = api.get("/targets")
    ctx.expect(r.status == 200, "the new token works", r.short())
    ctx.countdown(62, "waiting out the 60 s auth penalty so later checks aren't throttled")


@check("3.2", "PUT/GET target, /status, one-target limit")
def c_target(ctx: Ctx) -> None:
    api = ctx.ensure_token()
    body = ctx.ensure_target()
    ctx.ok(f"PUT /targets/{ctx.tid} saved the target")
    g = api.get(f"/targets/{ctx.tid}")
    ctx.expect(g.status == 200, f"GET /targets/{ctx.tid} -> 200", g.short())
    ctx.expect(g.get("board") == ctx.args.board, f"board {g.get('board')}")
    ctx.expect((g.get("mac") or "").upper() == ctx.mac, f"mac {g.get('mac')}")
    ctx.expect(g.get("manifest_url") == body.get("manifest_url"), "manifest_url round-trips")
    st = api.status()
    ids = [t.get("id") for t in (st or {}).get("targets", [])]
    ctx.expect(ctx.tid in ids, f"GET /status lists the target ({ids})")
    other = "other-target" if ctx.tid != "other-target" else "second-target"
    r = api.put(f"/targets/{other}", ctx.target_body())
    ctx.expect(r.status == 409 and r.error == "TARGET_LIMIT", f"PUT /targets/{other} -> 409 TARGET_LIMIT", r.short())


@check("3.3", "POST /ble/scan right after start ota", needs=("repeater",), after="3.5")
def c_scan(ctx: Ctx) -> None:
    api = ctx.ensure_token()
    ctx.ensure_target()
    ctx.start_ota(wait_enter=True)
    r = api.post("/ble/scan", {"duration_s": 10, "dfu_only": True}, timeout=40)
    ctx.expect(r.status == 200, "scan returned 200", r.short())
    devs = r.get("devices") or []
    ctx.data("devices", devs)
    for d in devs:
        ctx.note(f"{d.get('addr')} {d.get('name')!r} rssi {d.get('rssi')} dfu={d.get('dfu')} "
                 f"matches_target={d.get('matches_target')}")
    hit = [d for d in devs if d.get("matches_target") == ctx.tid]
    ctx.expect(hit, f"the repeater shows matches_target '{ctx.tid}'", f"{len(devs)} DFU advertisers seen")
    ctx.expect(hit[0].get("dfu") == "app_buttonless", "it shows dfu: app_buttonless", hit[0].get("dfu"))
    ctx.note("the repeater stays in OTA mode; the next flash may start without another `start ota`")


@check("3.4", "POST /targets/{id}/check")
def c_api_check(ctx: Ctx) -> None:
    api = ctx.ensure_token()
    ctx.ensure_target()
    ctx.ensure_staged()
    r = api.post(f"/targets/{ctx.tid}/check", timeout=60)
    ctx.expect(r.status == 200, "check -> 200", r.short())
    for key in ("update_available", "staged_same", "board_ok"):
        ctx.expect(isinstance(r.get(key), bool), f"{key} = {r.get(key)}")
    ctx.expect(r.get("version") == ctx.site.version, f"version {r.get('version')}")
    ctx.expect(r.get("board_ok") is True, "board_ok")
    ctx.expect(r.get("staged_same") is True, "staged_same: the staged image is this manifest's")
    ctx.ok(f"update_available={r.get('update_available')} (last_ok_version '{r.get('last_ok_version')}')")


@check("3.5", "happy path: POST /flash, `start ota` over LoRa", needs=("repeater",))
def c_happy_path(ctx: Ctx) -> None:
    api = ctx.ensure_token()
    ctx.ensure_target()
    w = ctx.api_flash_and_wait({"wifi_off": True}, lora=True)
    ctx.data("timeline", w.timeline)
    job = final_job(ctx, w)
    ctx.expect(w.saw("AWAITING_TARGET"), "the job armed (AWAITING_TARGET)")
    ctx.expect(job.get("state") == "SUCCEEDED", "the job SUCCEEDED", job.get("error"))
    ctx.soft(w.silent_s > 5, f"the API went quiet for {w.silent_s:.0f} s while WiFi was off")
    g = api.get(f"/targets/{ctx.tid}")
    ctx.expect(g.get("last_ok_version") == ctx.site.version, f"last_ok_version is {g.get('last_ok_version')}")
    lr = g.get("last_run") or {}
    ctx.expect(lr.get("seq") == job.get("run") and lr.get("result") == "SUCCEEDED",
               f"last_run is run #{job.get('run')}, SUCCEEDED", f"last_run seq {lr.get('seq')} {lr.get('result')}")


@check("3.6", "live progress on /status with wifi_off:false", needs=("repeater",))
def c_live_progress(ctx: Ctx) -> None:
    ctx.ensure_target()
    samples: list = []

    def on_sample(job: dict, src: str) -> None:
        if src == "api" and job.get("state") == "DFU_RUNNING":
            samples.append({k: job.get(k) for k in ("phase", "bytes_done", "bytes_total", "rate_bps", "attempt")})

    w = ctx.api_flash_and_wait({"wifi_off": False}, on_sample=on_sample, poll=0.5,
                               hint="(If it is still in OTA mode from the scan, the flash may start right away.)")
    job = final_job(ctx, w)
    ctx.expect(job.get("state") == "SUCCEEDED", "the job SUCCEEDED", job.get("error"))
    ctx.soft(w.silent_s == 0, "the API answered throughout the transfer")
    stream = [s for s in samples if s.get("bytes_done")]
    ctx.data("samples", samples[-50:])
    ctx.expect(len(stream) >= 3, f"{len(stream)} progress samples during the transfer")
    ctx.expect(all(s.get("phase") for s in samples), f"phase reported ({sorted({s['phase'] for s in samples})})")
    ctx.expect(stream[-1]["bytes_done"] > stream[0]["bytes_done"], "bytes_done grows")
    rates = [s["rate_bps"] for s in stream if s.get("rate_bps")]
    ctx.expect(rates, f"rate_bps reported (up to {max(rates or [0])} B/s)")


@check("3.7", "arm timeout -> TARGET_NOT_FOUND with a run record")
def c_arm_timeout(ctx: Ctx) -> None:
    api = ctx.ensure_token()
    ctx.ensure_target()
    ctx.ensure_repeater_idle()
    ctx.action("Do NOT send `start ota` now: this checks the 30 s arm timeout.")
    job, _ = ctx.api_flash({"arm_timeout_s": 30})
    w = ctx.wait_job(job, 120)
    ctx.expect(w.final.get("state") == "FAILED" and w.final.get("error", "").startswith("TARGET_NOT_FOUND"),
               "the job FAILED with TARGET_NOT_FOUND", f"{w.final.get('state')} {w.final.get('error')}")
    t_arm, t_end = w.time_of("AWAITING_TARGET"), w.time_of("FAILED")
    if t_arm is not None and t_end is not None:
        ctx.soft(20 <= t_end - t_arm <= 45, f"it gave up {t_end - t_arm:.0f} s after arming")
    run = w.final.get("run")
    ctx.expect(run, f"a run record was written (#{run})")
    r = api.get(f"/runs/{run}")
    rec = r.get("run") or {}
    ctx.expect(r.status == 200 and rec.get("result") == "FAILED", f"GET /runs/{run}: FAILED, {rec.get('error')}")


@check("3.8", "abort while armed -> FAILED ABORTED, repeater untouched")
def c_abort_armed(ctx: Ctx) -> None:
    api = ctx.ensure_token()
    ctx.ensure_target()
    ctx.ensure_repeater_idle()
    ctx.action("Do NOT send `start ota` now: this aborts a job that is still waiting.")
    job, _ = ctx.api_flash({"arm_timeout_s": 120})
    wait_state(ctx, job, {"AWAITING_TARGET"}, 120)
    r = api.post("/jobs/current/abort")
    ctx.expect(r.status == 202 and r.get("aborting") is True, "POST /jobs/current/abort -> 202", r.short())
    ctx.expect(r.get("target_erased") is False, "the reply says the target was not erased")
    w = ctx.wait_job(job, 60)
    ctx.expect(w.final.get("state") == "FAILED" and w.final.get("error") == "ABORTED", "the job ended FAILED, ABORTED",
               f"{w.final.get('state')} {w.final.get('error')}")
    ctx.expect(not w.final.get("target_erased"), "target_erased: false")


@check("3.9", "abort mid-transfer warns, then source:staged recovers", needs=("repeater",))
def c_abort_transfer(ctx: Ctx) -> None:
    api = ctx.ensure_token()
    ctx.ensure_target()
    ctx.ensure_staged()
    job, _ = ctx.api_flash({"wifi_off": False})
    wait_state(ctx, job, {"AWAITING_TARGET"}, 120)
    ctx.start_ota(still_needed=ctx.awaiting(job))
    end = time.monotonic() + ctx.args.start_ota_timeout + 180
    while True:
        st = api.status(timeout=3) or {}
        j = st.get("job") or {}
        if j.get("id") == job and j.get("state") in TERMINAL:
            raise TestFailed(f"the job ended ({j.get('state')}) before it could be aborted mid-transfer")
        if (j.get("id") == job and j.get("state") == "DFU_RUNNING" and j.get("bytes_total")
                and j.get("bytes_done", 0) >= j["bytes_total"] // 5):
            break
        if time.monotonic() > end:
            raise TestFailed("the transfer never reached 20%")
        time.sleep(0.5)
    ctx.say(f"transfer at {100 * j['bytes_done'] // j['bytes_total']}%: aborting")
    r = api.post("/jobs/current/abort")
    ctx.expect(r.status == 202, "abort -> 202", r.short())
    ctx.expect(r.get("target_erased") is True, "the reply says the target is erased")
    ctx.expect(bool(r.get("warning")), f"the reply warns: {r.get('warning', '')[:80]}…")
    w = ctx.wait_job(job, 120)
    ctx.expect(w.final.get("state") == "FAILED" and w.final.get("error") == "ABORTED", "the job ended FAILED, ABORTED",
               f"{w.final.get('state')} {w.final.get('error')}")
    w2 = ctx.api_flash_and_wait({"source": "staged", "wifi_off": False, "arm_timeout_s": 180}, start_ota=False)
    ctx.expect(w2.final.get("state") == "SUCCEEDED", "flash with source:staged recovered the repeater",
               w2.final.get("error"))


@check("3.10", "busy: 409s during a job; /status responsive during scan and check")
def c_busy(ctx: Ctx) -> None:
    api = ctx.ensure_token()
    ctx.ensure_target()
    ctx.ensure_repeater_idle()
    ctx.action("Do NOT send `start ota` now: this keeps a job armed to test the 409s.")
    job, _ = ctx.api_flash({"arm_timeout_s": 120})
    try:
        wait_state(ctx, job, {"AWAITING_TARGET"}, 120)
        r = api.post(f"/targets/{ctx.tid}/flash", {})
        ctx.expect(r.status == 409, "a second flash -> 409", r.short())
        r = api.post(f"/targets/{ctx.tid}/stage", {})
        ctx.expect(r.status == 409, "stage -> 409", r.short())
        r = api.post("/ble/scan", {"duration_s": 2}, timeout=20)
        ctx.expect(r.status == 409, "ble/scan -> 409", r.short())
        r = api.put(f"/targets/{ctx.tid}", ctx.target_body())
        ctx.expect(r.status == 409, "target PUT -> 409", r.short())
        out = ctx.con.cmd("scan 2", 20)
        ctx.expect("Busy:" in out, "console `scan` refuses: Busy", out.strip()[-120:])
    finally:
        api.post("/jobs/current/abort")
        ctx.wait_job(job, 60)

    def latency_during(label: str, fn, n: int) -> None:
        box: dict = {}
        th = threading.Thread(target=lambda: box.update(r=fn()), daemon=True)
        th.start()
        time.sleep(0.3)
        lat = []
        for _ in range(n):
            t0 = time.monotonic()
            ok = api.status(timeout=3) is not None
            lat.append(time.monotonic() - t0 if ok else None)
            time.sleep(0.7)
        th.join(45)
        r = box.get("r")
        ctx.expect(r is not None and r.status == 200, f"{label} completed with 200", r.short() if r else "no reply")
        ctx.expect(all(x is not None for x in lat) and max(lat) < 2.0,
                   f"/status answered during the {label} (max {max(x or 9 for x in lat) * 1000:.0f} ms)")

    latency_during("ble/scan (8 s)", lambda: api.post("/ble/scan", {"duration_s": 8}, timeout=40), 6)
    latency_during("check", lambda: api.post(f"/targets/{ctx.tid}/check", timeout=40), 2)


@check("3.11", "stage-only, force, bad hash and wrong board through the API")
def c_api_stage(ctx: Ctx) -> None:
    ctx.ensure_token()
    ctx.ensure_target()
    ctx.ensure_staged()
    n0 = ctx.site.count("good")
    w = ctx.wait_job(ctx.api_stage({}), 240)
    ctx.expect(w.final.get("state") == "STAGED", "POST /stage -> STAGED", w.final.get("error"))
    ctx.soft(w.final.get("reused") is True, "it reused the staged image")
    w = ctx.wait_job(ctx.api_stage({"force": True}), 240)
    ctx.expect(w.final.get("state") == "STAGED" and w.final.get("reused") is False, "{force:true} re-downloaded",
               w.final.get("error"))
    ctx.expect(ctx.site.count("good") == n0 + 1, "firmware.bin fetched once more")
    w = ctx.wait_job(ctx.api_stage({"force": True, "url": ctx.manifest_url("badhash")}), 240)
    ctx.expect(w.final.get("state") == "FAILED" and w.final.get("error", "").startswith("HASH_MISMATCH"),
               "bad sha256 -> FAILED HASH_MISMATCH", f"{w.final.get('state')} {w.final.get('error')}")
    w = ctx.wait_job(ctx.api_stage({"force": True}), 240)
    ctx.expect(w.final.get("state") == "STAGED", "re-staged the good image")
    try:
        ctx.ensure_target(board=WRONG_BOARD)
        job, _ = ctx.api_flash({"arm_timeout_s": 30})
        w = ctx.wait_job(job, 120)
        ctx.expect(w.final.get("state") == "FAILED" and w.final.get("error", "").startswith("BOARD_MISMATCH"),
                   "wrong board, source manifest -> FAILED BOARD_MISMATCH", f"{w.final.get('error')}")
        ctx.expect(not w.saw("AWAITING_TARGET"), "it never armed, so the repeater wasn't touched")
        job, _ = ctx.api_flash({"source": "staged", "arm_timeout_s": 30})
        w = ctx.wait_job(job, 120)
        ctx.expect(w.final.get("state") == "FAILED" and w.final.get("error", "").startswith("BOARD_MISMATCH"),
                   "wrong board, source staged -> FAILED BOARD_MISMATCH (from the staged header)",
                   f"{w.final.get('error')}")
        ctx.expect(not w.saw("AWAITING_TARGET"), "it never armed")
    finally:
        ctx.ensure_target()


@check("3.13", "logs cursor, PUT /config hostname, reboot")
def c_logs_config(ctx: Ctx) -> None:
    api = ctx.ensure_token()
    r = api.get("/logs")
    ctx.expect(r.status == 200 and isinstance(r.get("next"), int), f"GET /logs -> next={r.get('next')}")
    nxt = r.get("next")
    api.get("/targets", token="log-marker-wrong-token", retry_429=False)  # logs a "rejected request" warning
    time.sleep(0.5)
    r = api.get(f"/logs?since={nxt}")
    ctx.expect(r.status == 200 and r.get("next", 0) >= nxt, f"GET /logs?since={nxt} -> next={r.get('next')}")
    ctx.expect("rejected request" in (r.get("text") or ""), "it returns only the new lines (the rejected request)")
    orig = (api.get("/config").body or {}).get("hostname", "")
    r = api.put("/config", {"hostname": "bench-dfu"})
    ctx.expect(r.status == 200 and r.get("reboot_required") is True, "PUT /config hostname -> reboot_required",
               r.short())
    try:
        _reboot_via_api(ctx)
        st = ctx.ensure_wifi(wait=60)
        ctx.expect(st["hostname"] == "bench-dfu", f"it came back as {st['hostname']}")
        dev = ctx.wait_api()["device"]
        ctx.expect(dev.get("hostname") == "bench-dfu", "/status shows the new hostname")
        ip = ctx.resolve("bench-dfu.local")
        ctx.soft(ip == st["ip"], f"bench-dfu.local resolves to {st['ip']}", f"got {ip}")
    finally:
        if not ctx.args.keep_hostname:
            ctx.ensure_token().put("/config", {"hostname": orig})
            _reboot_via_api(ctx)
            ctx.ensure_wifi(wait=60)
            ctx.note(f"hostname restored to {orig or 'the default'}")


def _reboot_via_api(ctx: Ctx) -> None:
    boots = len(ctx.con.boots)
    r = ctx.ensure_token().post("/system/reboot")
    ctx.expect(r.status == 202, "POST /system/reboot -> 202", r.short())
    ctx.after_boot(ctx.con.wait_boot(boots, 60))


@check("3.14", "run records carry trigger, source, target_id, started_unix")
def c_run_fields(ctx: Ctx) -> None:
    api = ctx.ensure_token()
    ctx.ensure_target()
    runs = api.get(f"/targets/{ctx.tid}/runs").get("runs") or []
    first = ctx.first_seq or 0
    new = [r for r in runs if r.get("seq", 0) >= first and r.get("result") != "ABORTED_BY_REBOOT"]
    if not new:
        raise TestSkipped("no flash runs recorded in this session yet")
    synced = ((api.status() or {}).get("device", {}).get("unix_time", 0) or 0) > 1_600_000_000
    bad = []
    rows = ["| run | trigger | source | target_id | started_unix | result |", "|---|---|---|---|---|---|"]
    for r in sorted(new, key=lambda r: r["seq"]):
        rows.append(f"| {r['seq']} | {r.get('trigger')} | {r.get('source')} | {r.get('target_id')} | "
                    f"{r.get('started_unix')} | {r.get('result')} |")
        if r.get("trigger") not in ("api", "console") or r.get("source") not in ("manifest", "staged") \
                or r.get("target_id") != ctx.tid:
            bad.append(r["seq"])
    ctx.data("table", rows)
    ctx.expect(not bad, f"{len(new)} runs from this session have trigger, source and target_id", f"bad: {bad}")
    # Console runs in section 1 happen before 2.1 joins WiFi, so their clock isn't set yet;
    # an API job needs WiFi, so by then SNTP should have synced.
    zero_console = [r["seq"] for r in new if not r.get("started_unix") and r.get("trigger") == "console"]
    zero_api = [r["seq"] for r in new if not r.get("started_unix") and r.get("trigger") == "api"]
    newest = max(new, key=lambda r: r["seq"])
    if synced:
        ctx.expect(newest.get("started_unix"), f"the newest run (#{newest['seq']}) has started_unix")
        ctx.soft(not zero_api, "every API-started run has a non-zero started_unix", f"zero on {zero_api}")
        if zero_console:
            ctx.note(f"console runs {zero_console} have started_unix 0: they ran before the clock was set (no WiFi "
                     "yet), which is expected")
    else:
        ctx.warn("the clock isn't synced (SNTP), so started_unix can't be checked")


# ============================================================================================
# 4. Safety layers (debug build)
# ============================================================================================


def enter_debug(ctx: Ctx) -> Optional[str]:
    """Section 4 setup: flash the debug build if given; check the fault endpoint exists."""
    if ctx.args.debug_app_bin:
        lay = ctx.layout
        if not lay:
            return "flash layout unknown: pass --layout 4mb or 8mb"
        print(f"\n  flashing the debug build at 0x{lay.app:X}", flush=True)
        ctx.flash_images([(lay.app, Path(ctx.args.debug_app_bin))])
    try:
        api = ctx.ensure_token()
    except (TestSkipped, TestFailed) as e:
        return str(e)
    r = api.post("/debug/fault", {})
    if r.status == 404:
        return ("debug endpoints aren't built in: flash a CONFIG_DFU32_DEBUG_ENDPOINTS=y build "
                "(--debug-app-bin, see docs/BENCH.md §7)")
    if r.status != 200:
        return f"POST /debug/fault failed: {r.short()}"
    ctx.debug_build = True
    return None


def leave_debug(ctx: Ctx) -> None:
    if ctx.args.debug_app_bin and ctx.args.app_bin and ctx.layout:
        print(f"\n  restoring the normal build at 0x{ctx.layout.app:X}", flush=True)
        ctx.flash_images([(ctx.layout.app, Path(ctx.args.app_bin))])
        ctx.debug_build = False
    elif ctx.debug_build:
        print("\n  NOTE: the debug build (fault endpoints) is still on DFU32; pass --app-bin to restore the normal one",
              flush=True)


def _arm_fault(ctx: Ctx, fault: dict):
    api = ctx.ensure_token()
    ctx.ensure_target()
    ctx.ensure_staged()
    r = api.post("/debug/fault", fault)
    ctx.expect(r.status == 200, f"POST /debug/fault {fault}", r.short())
    return api


@check("4.1", "restore watchdog: restore_fail -> retry, then reboot", needs=("repeater",))
def c_restore_fail(ctx: Ctx) -> None:
    _arm_fault(ctx, {"restore_fail": True})
    mark, boots = ctx.con.mark(), len(ctx.con.boots)
    job, _ = ctx.api_flash({"source": "staged", "wifi_off": True})
    wait_state(ctx, job, {"AWAITING_TARGET"}, 120)
    ctx.start_ota(still_needed=ctx.awaiting(job))
    t = ctx.args.start_ota_timeout + 400
    a = ctx.con.wait_line(r"fault injection: treating WiFi restore as failed", t, since=mark)
    ctx.expect(a, "the transfer finished and the restore fault fired")
    b = ctx.con.wait_line(r"after restore; retrying once", 120, since=a[0])
    ctx.expect(b, "first restore 'failed': retrying once")
    c = ctx.con.wait_line(r"rebooting so the network comes back", 120, since=b[0])
    ctx.expect(c, "second restore 'failed': rebooting")
    boot = ctx.con.wait_boot(boots, 60)
    ctx.after_boot(boot)
    d1, d2 = b[1].t - a[1].t, c[1].t - b[1].t
    ctx.data("waits_s", [round(d1, 1), round(d2, 1)])
    ctx.note(f"restore waits: {d1:.0f} s, then {d2:.0f} s before the reboot (each ends early once WiFi is up, "
             f"60 s at most)")
    ctx.soft(d1 <= 75 and d2 <= 75, "each restore wait stayed within the 60 s limit")
    ctx.expect("interrupted by a reboot" not in ctx.con.boot_lines(boot),
               "no ABORTED_BY_REBOOT: the run was recorded before the restore")
    st = ctx.ensure_wifi(wait=60)
    ctx.expect(st["connected"], "WiFi is back after the reboot")
    lr = ctx.ensure_token().get(f"/targets/{ctx.tid}").get("last_run") or {}
    ctx.expect(lr.get("result") == "SUCCEEDED", f"the run record (#{lr.get('seq')}) was written: SUCCEEDED")


@check("4.2", "run deadline: deadline_s 20 -> DEADLINE_EXCEEDED, WiFi back", needs=("repeater",))
def c_deadline(ctx: Ctx) -> None:
    _arm_fault(ctx, {"deadline_s": 20})
    mark = ctx.con.mark()
    w = ctx.api_flash_and_wait({"source": "staged", "wifi_off": True})
    job = final_job(ctx, w)
    ctx.expect(job.get("state") == "FAILED" and job.get("error", "").startswith("DEADLINE_EXCEEDED"),
               "the job FAILED with DEADLINE_EXCEEDED", f"{job.get('state')} {job.get('error')}")
    seen = ctx.con.find_line(r"target seen; WiFi", since=mark)
    hit = ctx.con.find_line(r"run deadline exceeded", since=mark)
    if seen and hit:
        d = hit[1].t - seen[1].t
        ctx.soft(17 <= d <= 30, f"aborted {d:.0f} s after the target was seen")
    ctx.ok("WiFi came back (the API answers)")
    r = ctx.ensure_token().get(f"/runs/{job.get('run')}")
    rec = r.get("run") or {}
    ctx.expect(rec.get("error", "").startswith("DEADLINE_EXCEEDED"), f"run #{job.get('run')} records DEADLINE_EXCEEDED")
    if job.get("target_erased"):
        ctx.recover_target_api()


@check("4.3", "stall watchdog: stall_s 40 -> reboot, ABORTED_BY_REBOOT", needs=("repeater",))
def c_stall(ctx: Ctx) -> None:
    _arm_fault(ctx, {"stall_s": 40})
    mark, boots = ctx.con.mark(), len(ctx.con.boots)
    job, _ = ctx.api_flash({"source": "staged", "wifi_off": True})
    wait_state(ctx, job, {"AWAITING_TARGET"}, 120)
    ctx.start_ota(still_needed=ctx.awaiting(job))
    a = ctx.con.wait_line(r"fault injection: blocking the DFU task", ctx.args.start_ota_timeout + 60, since=mark)
    ctx.expect(a, "the DFU task blocked without a heartbeat")
    b = ctx.con.wait_line(r"no sign of life for \d+ s: rebooting", 90, since=a[0])
    ctx.expect(b, "the stall watchdog fired")
    d = b[1].t - a[1].t
    ctx.soft(25 <= d <= 45, f"it rebooted {d:.0f} s after the stall began (expect ~30-35)")
    boot = ctx.con.wait_boot(boots, 60)
    ctx.after_boot(boot)
    ctx.expect("interrupted by a reboot" in ctx.con.boot_lines(boot), "the next boot logs the interrupted run")
    rows = ctx.runs()
    ctx.expect(rows and rows[0]["result"] == "ABORTED_BY_REBOOT", "newest run is ABORTED_BY_REBOOT")
    if rows[0]["erased"] == "possibly":
        ctx.recover_target()
    else:
        ctx.note("the repeater was never connected; it may still be in OTA mode (it times out or the next "
                 "flash picks it up)")


# ============================================================================================
# 5. WiFi on vs off comparison
# ============================================================================================


@check("5.1", "WiFi on vs off during the transfer", needs=("repeater",))
def c_wifi_compare(ctx: Ctx) -> None:
    n = ctx.args.compare_runs
    if n <= 0:
        raise TestSkipped("--compare-runs 0")
    api = ctx.ensure_token()
    ctx.ensure_target()
    ctx.ensure_staged()
    rows: list = []
    for i in range(2 * n):
        off = i % 2 == 0
        ctx.say(f"--- comparison run {i + 1}/{2 * n}: wifi_off={str(off).lower()}")
        w = ctx.api_flash_and_wait({"source": "staged", "wifi_off": off})
        job = final_job(ctx, w)
        rec = (api.get(f"/runs/{job.get('run')}").get("run") or {}) if job.get("run") else {}
        rows.append({"i": i + 1, "wifi_off": off, "state": job.get("state"), "run": job.get("run"), "rec": rec})
        if job.get("state") != "SUCCEEDED" and job.get("target_erased"):
            ctx.recover_target_api()
        if job.get("state") != "SUCCEEDED" and ctx.repeater_alive() is False:
            # A hung repeater fails every remaining run after a full arm timeout each: stop here.
            ctx.warn("the repeater stopped answering on its USB console after run "
                     f"{i + 1}: it looks hung. Power-cycle it, then re-run 5.1")
            break
    table = ["| # | wifi_off | result | duration | stream rate | attempts | failure kinds |",
             "|---|---|---|---|---|---|---|"]
    with open(ctx.out / "wifi_compare.csv", "w", newline="") as f:
        cw = csv.writer(f)
        cw.writerow(["n", "wifi_off", "run", "result", "duration_ms", "rate_kBps", "attempts", "failure_kinds",
                     "error"])
        for r in rows:
            rec = r["rec"]
            atts = rec.get("attempts", [])
            kinds = ",".join(a.get("kind", "") for a in atts if a.get("kind") not in ("NONE", ""))
            rate = (rec.get("rate_x10_kBps") or 0) / 10
            cw.writerow([r["i"], r["wifi_off"], r["run"], rec.get("result", r["state"]), rec.get("duration_ms"),
                         rate, len(atts), kinds, rec.get("error", "")])
            table.append(f"| {r['i']} | {str(r['wifi_off']).lower()} | {rec.get('result', r['state'])} | "
                         f"{(rec.get('duration_ms') or 0) / 1000:.1f} s | {rate:.1f} kB/s | {len(atts)} | {kinds} |")
    table += ["", "| wifi_off | runs | succeeded | mean rate | mean duration | extra attempts |", "|---|---|---|---|---|---|"]
    for off in (True, False):
        grp = [r for r in rows if r["wifi_off"] == off]
        ok = [r for r in grp if r["rec"].get("result") == "SUCCEEDED"]
        rates = [r["rec"].get("rate_x10_kBps", 0) / 10 for r in ok if r["rec"].get("rate_x10_kBps")]
        durs = [r["rec"].get("duration_ms", 0) / 1000 for r in ok]
        extra = sum(max(0, len(r["rec"].get("attempts", [])) - 1) for r in grp)
        table.append(f"| {str(off).lower()} | {len(grp)} | {len(ok)} | "
                     f"{statistics.mean(rates) if rates else 0:.2f} kB/s | {statistics.mean(durs) if durs else 0:.1f} s | "
                     f"{extra} |")
        ctx.note(f"wifi_off={str(off).lower()}: {len(ok)}/{len(grp)} succeeded, mean "
                 f"{statistics.mean(rates) if rates else 0:.2f} kB/s, {extra} extra attempts")
    ctx.data("table", table)
    print("\n" + "\n".join("    " + t for t in table), flush=True)
    failed = [r["i"] for r in rows if r["rec"].get("result") != "SUCCEEDED"]
    ctx.soft(not failed, f"all {2 * n} runs succeeded", f"failed: {failed}")


HOOKS = {"enter": {"4": enter_debug}, "leave": {"4": leave_debug}}
