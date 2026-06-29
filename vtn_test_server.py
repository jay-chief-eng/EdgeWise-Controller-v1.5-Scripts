"""
vtn_test_server.py - OpenADR 2.0 VTN for Opta test protocol
================================================================
Runs on the laptop at 10.0.0.100:8080

Test Sequence:
  1. Opta registers - VTN confirms venID ven1234
  2. User presses Enter to send SIMPLE signal level 1 (light shed)
     Opta receives on next poll, reports 3 kW shed
  3. User presses Enter to send SIMPLE signal level 2 (moderate shed)
     Opta recives on next poll, reports 10 kW shed

Requirements:
    pip install openleadr

Run: python vtn_test_server.py
"""

import time
import asyncio
import traceback
import logging
import threading
from datetime import datetime, timezone, timedelta
from aiohttp import web
from openleadr import OpenADRServer, server
import nest_asyncio
nest_asyncio.apply()

# --Configuration Section-------------------------------------

# Logging
logging.basicConfig(
    level=logging.DEBUG,
    format="%(asctime)s %(levelname)-8s %(message)s",
    datefmt="%H:%M:%S"
)
log = logging.getLogger("VTN")
logging.getLogger('openleadr').setLevel(logging.DEBUG)
logging.getLogger('aiohttp').setLevel(logging.DEBUG)

# Global variables
VTN_ID          = "test-vtn"
EXPECTED_VEN    = "ven1234"
HOST            = "10.0.0.100"
PORT            = 8080

# Shared state
registration_event = threading.Event()
report_event       = threading.Event()
event_queue = asyncio.Queue()
registered_ven_id = None
server_ref        = None
last_report_value = None

# --Function call definitions-------------------------------

# Event queue processor
async def process_event_queue(server):
    """
    Runs inside the event loop. Picks up event requests
    from the console thread and submits them to openleadr.
    """
    log.info("Event queue processor started.")
    while True:
        try:
            signal_level = event_queue.get_nowait()
            log.info(f"Queue processor: got signal level {signal_level}")
            await send_simple_event(server, signal_level=signal_level)
            log.info(f"Queue processor: send_simple_event completed.")
        except asyncio.QueueEmpty:
            pass
        except Exception as e:
            log.error(f"Queue processor error: {e}")
            log.error(traceback.format_exc())
        await asyncio.sleep(0.5)

# Registration handler
async def on_create_party_registration(registration_info):
    # Print the full registration_info to see all available keys
    log.info(f"Full registration_info: {registration_info}")
    
    ven_name = registration_info.get("ven_name", "unknown")
    log.info(f"Registration request from VEN: {ven_name}")

    if str(ven_name) == str(EXPECTED_VEN):
        log.info(f"VEN {EXPECTED_VEN} recognised. Registration confirmed.")
        global registered_ven_id
        registered_ven_id = ven_name
        log.info(f"registered_ven_id set to: {registered_ven_id} "
                 f"type: {type(registered_ven_id)}")
        registration_event.set() # signal the console thread
        return EXPECTED_VEN, "reg-id-001"
    else:
        log.warning(f"Unexpected VEN name: {ven_name}. Rejecting.")
        log.warning(f"Expected: '{EXPECTED_VEN}' type: {type(EXPECTED_VEN)}")
        log.warning(f"Got: '{ven_name}' type: {type(ven_name)}")
        return None

async def on_register_report(ven_id, resource_id, measurement, 
                              unit, scale, min_sampling_interval, 
                              max_sampling_interval):
    log.info(f"Report registration from VEN: {ven_id}")
    log.info(f"  Resource: {resource_id}")
    log.info(f"  Measurement: {measurement}")
    # Return a callback and sampling interval to accept the report
    # Return None to decline
    callback = on_update_report
    sampling_interval = timedelta(seconds=10)
    return callback, sampling_interval

# Report handler
def waitForReport(timeout=30, expected_value=None):
    """
    Waits for a compliance report from the Opta.
    Returns True if report received within timeout, False otherwise.
    Prints debug information if no report arrives.
    """

    # Clear any previous report signal before waiting
    report_event.clear()

    print(f"  Waiting up to {timeout} seconds for compliance report...")

    start_time = time.time()
    received = report_event.wait(timeout=timeout)

    if received:
        elapsed = round(time.time() - start_time, 1)
        print(f"  Report received after {elapsed} seconds.")
        print(f"  Reported value : {last_report_value} kW")
        if expected_value is not None:
            if str(last_report_value == str(expected_value)):
                print(f"  Expected value : {expected_value} kW - PASS")
            else:
                print(f"  Expected value : {expected_value} kW - FAIL")
                print(f"  WARNING: reported value does not match expected value.")
        return True
    else:
        elapsed = round(time.time() - start_time, 1)
        print(f"  No report received after {elapsed} seconds.")
        print("  Debug checlist:")
        print("    1. Check Opta Serial Monitor - did it receive the event?")
        print("    2. Check Opta Serial Monitor - did it send a report?")
        print("    3. Check VTN log - was the EiReport POST received?")
        print("    4. Is the opta still polling? (watch VTN log for OadrPoll)")
        return False

# Report handler
async def on_update_report(report):
    """
    Called whenthe Opta send oadrUpdateReport (compliance report)
    Prints the reported load shed value
    """
    global last_report_value
    log.info("-" * 50)
    log.info("Compliance report received from VEN:")
    for item in report.get("intervals", []):
        value = item.get("value")
        log.info(f"  Reported load shed value : {value} kW")
        last_report_value = value
    log.info("-" * 50)
    report_event.set()  # signal the console thread

# Event response handler
async def on_event_response(ven_id, event_id, opt_type, **kwargs):
    """
    Called when the Opta sends oadrCreated event (acknolwedgement/echo)
    """
    log.info(f"Event acknowledgedment from VEN {ven_id}:")
    log.info(f"  Event ID  : {event_id}")
    log.info(f"  Opt type  : {opt_type}")
    if opt_type == "optIn":
        log.info("  VEN confirmed compliance (optIn).")
    else:
        log.info("  VEN did not confirm compliance (optOut).")

# Poll handler

async def on_poll(ven_id):
    try:
        log.info(f"Poll received from VEN: {ven_id}")
        return []
    except Exception as e:
        log.error(f"Poll handler error: {e}")
        log.error(traceback.format_exc())
        raise

# Send a SIMPLE signal event
async def send_simple_event(server, signal_level: int,
                            duration_minutes: int = 10):
    """
    Publishes an oadrDistributeEvent with a SIMLE signal.
    signal_level: 0 = normal, 1=light shed, 2=moderate shed, 3=emergency
    """
    labels = {0: "Normal", 1: "Light shed",
              2: "Moderate shed", 3: "Emergency shed"}
    label  = labels.get(signal_level, "Unknown")

    log.info(f"send_simple_event called: level {signal_level} ({label})")
    log.info(f"Targeting VEN ID: {registered_ven_id} type: {type(registered_ven_id)}")

    try:
        await server.add_event(
            ven_id        = registered_ven_id,
            signal_name   = "SIMPLE",
            signal_type   = "SIMPLE",
            target        = {"ven_id": registered_ven_id},
            intervals     = [{
                "dtstart"        : datetime.now(timezone.utc) - timedelta(minutes=5),
                "duration"       : timedelta(minutes=duration_minutes),
                "signal_payload" : signal_level
            }],
            callback      = on_event_response
        )
        log.info(f"server.add_event completed successfully.")
        log.info(f"Server attributes: {[a for a in dir(server) if not a.startswith('_')]}")
        log.info(f"Server __dict__: {server.__dict__.keys()}")
        log.info(f"message_queues keys: {list(server.message_queues.keys())}")
        log.info(f"message_queues content: {server.message_queues}")

    except Exception as e:
        log.error(f"server.add_event failed: {e}")
        log.error(traceback.format_exc())

# Console input
def console(server, loop):
    """
    Runs in seperate thread. Waits for user input to trigger
    events so the asyncio server loop is not blocked.
    """
    print("\nConsole thread started. Waiting for Opta to register...")

    # Wait up to 5 minutes for registration
    registered = registration_event.wait(timeout=300)

    if not registered:
        print("Timed out waiting for registration.")
        return
    
    print(f"\nOpta registered. Ready for test sequence.")
    print("\nPress Enter to send SIMPLE level 1 (moderate shed)...")
    input()

    print("Submitting level 1 event to event loop...")
    print("Queueing level 1 event...")
    loop.call_soon_threadsafe(event_queue.put_nowait, 1)
    print("Event queued.")
    
    # Wait for compliance report from Opta
    print("Waiting for Opta compliance report...")
    received = waitForReport(timeout=30, expected_value = 3)
    if received:
        print("\n>>> Screenshot opportunity: level 1 report received.")
        print(">>> Press Enter when ready to continue...")
        input()

    print("\nPress Enter to send SIMPLE level 2 (heavy shed)...")
    input()

    print("Queueing level 2 event...")
    loop.call_soon_threadsafe(event_queue.put_nowait, 2)
    print("Event queued.")
    
    # Wait for compliance report from Opta
    print("Waiting for Opta compliance report...")
    received = waitForReport(timeout=30, expected_value = 10)
    if received:
        print("\n>>> Screenshot opportunity: level 2 report received.")
        print(">>> Press Enter when ready to continue...")
        input()

    print("\nTest sequence complete. Press Ctrl+C to stop the server.")

# Middleware error handler
@web.middleware
async def error_middleware(request, handler):
    try:
        response = await handler(request)
        return response
    except Exception as e:
        log.error(f"Unhandled exception on {request.path}: {e}")
        log.error(traceback.format_exc())
        raise

# --Main Program Loop-----------------------------------------
async def main():
    log.info("=" * 50)
    log.info(f"OpenADR VTN starting")
    log.info(f"  VTN ID     : {VTN_ID}")
    log.info(f"  Host       : {HOST}:{PORT}")
    log.info(f"  Opta IP    : 10.0.0.101")
    log.info("=" * 50)

    server = OpenADRServer(
        vtn_id      = VTN_ID,
        http_host   = HOST,
        http_port   = PORT
    )

    server.add_handler("on_create_party_registration",
                       on_create_party_registration)
    server.add_handler("on_update_report", on_update_report)
    server.add_handler("on_register_report", on_register_report)

    global server_ref
    server_ref = server

    # Start server without blocking the event loop
    await server.run_async()
    log.info("Server started.")

    # Start console thread — server is now ready
    loop = asyncio.get_running_loop()
    console_thread = threading.Thread(
        target=console,
        args=(server, loop),
        daemon=True
    )
    console_thread.start()
    log.info("Console thread started.")

    # Start queue processor as a task
    asyncio.ensure_future(process_event_queue(server))
    log.info("Queue processor started.")

    # Keep running until interrupted
    try:
        while True:
            await asyncio.sleep(1)
    except asyncio.CancelledError:
        log.info("Server shutting down.")
        await server.stop()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        log.info("VTN stopped.")
