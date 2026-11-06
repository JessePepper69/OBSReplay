# Module contract

Settings JSON keys: `obs_path`, `clips_dir`, `clip_seconds` (30..600), `capture` (`game`/`desktop`), `monitor_id`, `output_height` (0/720/1080/1440), `fps` (30/60/120/144), `encoder` (`auto`/`nvidia`/`amd`/`intel`/`cpu`), `codec` (`h264`/`hevc`/`av1`), `quality` (16/20/24), `mic_enabled`, `mic_device`, `desktop_device` (`default`/`off`/device ID), `noise` (`off`/`rnnoise`/`speex`), `separate_tracks`, `sort_by_app`, `save_sound`, `start_at_login`, `save_hotkey`, `show_hotkey`, `replay_enabled`.

`Controller::snapshot()` is a quick, mutex-protected JSON string. Keys: `settings`, `options`, `integration`, `obs_found`, `obs_running`, `connected`, `owned`, `replay_active`, `busy`, `operation`, `status`, `message`, `error`, `revision`, `clips`. Clips: `path`, `name`, `app`, `bytes`, `saved_at`.

Options keys: `monitors` (`id`, `label`, `width`, `height`), `audio_inputs`/`audio_outputs` (`id`, `label`), `encoders` (`id`, `label`, `codecs` array). Hardware probes run on worker, never snapshot/UI thread.

`dispatch(action,payload)` accepts an action into a bounded serialized native queue, or returns an error. Acceptance does not mean completion. The frontend observes `busy`, `message`, `error`, `revision` and actual state. Actions: `setup`, `apply` (payload is entire settings object), `start`, `stop`, `save`, `reconnect`, `reset`, `open_folder`, `open_obs`, `open_clip`/`reveal_clip` (payload `path`), `help` (payload `topic`), `refresh_options`. No arbitrary shell commands or URLs.

Main uses `saucer.exposed.snapshot()` and `dispatch(action,JSON.stringify(payload))`. `pick_folder(current)`/`pick_obs(current)` are native UI dialogs returning selected path or empty string. `quit()` exits controller while OBS stays active; `hide()` hides to tray. Main owns hotkey validation/registration on the UI thread and rejects conflicting settings submissions before queuing them. On failed apply, resynchronize bindings from persisted snapshot settings.

Setup returns resolved settings from Integration::prepare (e.g. actual encoder selection). It commits the dedicated profile, collection, shared WebSocket changes and resolved app settings in one rollback-capable transaction while OBS is closed. The controller updates its in-memory snapshot after that succeeds. Launch and buffer start are distinct operations. All platform code uses UTF-8 JSON and native wide paths. Dependencies are fetched at pinned versions; reference C# code and assets are not copied.
