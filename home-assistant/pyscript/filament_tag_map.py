# pyscript/filament_tag_map.py  VERSION=9
import json
import os
from pathlib import Path

VERSION = 9

def _get_file_path():
    return hass.config.path("filament_tag_map.json")

def _load_map():
    file_path = Path(_get_file_path())
    try:
        if not file_path.exists():
            log.warning(f"[RFID] v{VERSION} File not found. Starting fresh.")
            return {}
            
        # read_text() opens, reads, and closes the file instantly in one move
        content = file_path.read_text(encoding="utf-8").strip()
        
        if not content:
            log.warning(f"[RFID] v{VERSION} File exists but is empty.")
            return {}
            
        data = json.loads(content)
        return data if isinstance(data, dict) else {}
    except Exception as exc:
        log.error(f"[RFID] v{VERSION} Error reading map: {exc}")
        return {}

def _write_map(tag_map):
    file_path = Path(_get_file_path())
    try:
        json_str = json.dumps(tag_map, indent=2)
        
        # write_text() opens, writes, and closes instantly, bypassing async delays
        file_path.write_text(json_str, encoding="utf-8")
        
        # Verify immediately with a fresh read
        verify = file_path.read_text(encoding="utf-8").strip()
        
        if verify:
            log.info(f"[RFID] v{VERSION} SUCCESS: {len(tag_map)} entries written and verified via pathlib.")
        else:
            log.error(f"[RFID] v{VERSION} CRITICAL: pathlib write failed silently.")
            
    except Exception as exc:
        log.error(f"[RFID] v{VERSION} Error writing map: {exc}")
        raise

def _normalise(raw):
    uid = str(raw).replace("-", "").replace(":", "").upper()
    return (uid + "0000000000000000")[:16]

def _upsert(spool_id, uid, label):
    tag_map = _load_map()
    
    for k in [k for k, v in tag_map.items()
              if str(v.get("spoolman_id")) == str(spool_id) and k != uid]:
        del tag_map[k]
        log.info(f"[RFID] v{VERSION} removed stale UID {k} for spool {spool_id}")
        
    tag_map[uid] = {"friendly_name": label, "spoolman_id": int(spool_id)}
    _write_map(tag_map)
    return len(tag_map)

@service
def save_filament_tag_map(spool_id=None, tag_uid=None, friendly_name=None):
    log.info(f"[RFID] v{VERSION} save called — spool_id={spool_id!r} tag_uid={tag_uid!r}")

    if not spool_id or not tag_uid:
        log.error(f"[RFID] v{VERSION} missing spool_id or tag_uid — aborting")
        return

    try:
        sid = int(spool_id)
        assert sid > 0
    except Exception:
        log.error(f"[RFID] v{VERSION} invalid spool_id: {spool_id!r}")
        return

    uid = _normalise(tag_uid)
    label = str(friendly_name) if friendly_name else f"Spoolman ID {sid}"

    try:
        count = _upsert(sid, uid, label)
        log.info(f"[RFID] v{VERSION} OK — {uid} → spool {sid} ({label}), {count} entries")
    except Exception as exc:
        log.error(f"[RFID] v{VERSION} write failed: {exc}")

@service
def register_rfid_tag(spool_id=None, tag_uid=None, friendly_name=None):
    save_filament_tag_map(spool_id=spool_id, tag_uid=tag_uid, friendly_name=friendly_name)

@service
def ping_pyscript():
    log.warning(f"[RFID] PING — VERSION={VERSION} loaded. True path: {_get_file_path()}")