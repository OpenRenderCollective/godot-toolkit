extends SceneTree

const Stream = preload("res://scripts/json_stream.gd")
const Client = preload("res://scripts/renderer_client.gd")
const Sessions = preload("res://scripts/project_sessions.gd")
const Geometry = preload("res://scripts/layout_geometry.gd")
class FailedScene extends RefCounted:
	func execute_data(_request: String) -> Dictionary:
		return {"ok": false, "error": "Simulated scene failure"}
	func start_asset_preview(_path: String) -> String:
		return '{"ok":false,"error":"Preview disabled"}'
	func cancel_asset_preview() -> void: pass

var failures := 0
var directory := ""

func check(ok: bool, message: String) -> void:
	if not ok: push_error(message); failures += 1

func _initialize() -> void: run.call_deferred()

func read_chain(file: FileAccess, limit: int) -> PackedByteArray:
	var bytes := PackedByteArray()
	while file.get_position() < file.get_length():
		var flags := file.get_32()
		var count := flags & ~Stream.MORE_FRAMES
		check(count > 0 and count <= limit, "Frame length must be bounded and nonzero")
		bytes.append_array(file.get_buffer(count))
		if not flags & Stream.MORE_FRAMES: break
	return bytes

# Packed arrays compare by type name and bytes; other values through JSON.
func expected(value: Variant) -> Variant:
	var type := Stream.new()._packed(value)
	if not type.is_empty(): return {"type": type, "bytes": value if value is PackedByteArray else value.to_byte_array()}
	if value is Dictionary:
		var result := {}
		for key in value: result[key] = expected(value[key])
		return result
	if value is Array: return value.map(expected)
	return JSON.parse_string(JSON.stringify([value]))[0]

func decoded(value: Variant, blob: PackedByteArray, offsets: Array, sizes: Array) -> Variant:
	if value is Dictionary and value.has("$buffer"):
		var i := int(value["$buffer"])
		return {"type": value.type, "bytes": blob.slice(offsets[i], offsets[i] + int(sizes[i]))}
	if value is Dictionary:
		var result := {}
		for key in value: result[key] = decoded(value[key], blob, offsets, sizes)
		return result
	if value is Array: return value.map(func(item): return decoded(item, blob, offsets, sizes))
	return value

func roundtrip(message: Dictionary, limit: int) -> void:
	var path := directory.path_join("framed.bin")
	var file := FileAccess.open(path, FileAccess.WRITE_READ)
	check(Stream.new().write(file, message, limit), "Stream writer must succeed")
	file.seek(0)
	var json := JSON.new()
	check(json.parse(read_chain(file, limit).get_string_from_utf8()) == OK, "Fragmented payload must be valid UTF-8 JSON")
	var data: Dictionary = json.data if json.data is Dictionary else {}
	var sizes: Array = data.get("buffers", [])
	data.erase("buffers")
	var offsets := []
	var total := 0
	for size in sizes: offsets.append(total); total += int(size)
	var blob := read_chain(file, limit) if total > 0 else PackedByteArray()
	check(blob.size() == total and file.get_position() == file.get_length(), "Binary buffers must form one chain of their declared total size")
	check(decoded(data, blob, offsets, sizes) == expected(message), "All scene values must survive fragmentation")

func run() -> void:
	directory = ProjectSettings.globalize_path("res://../.work/large-scene/%d" % Time.get_ticks_usec()).simplify_path()
	DirAccess.make_dir_recursive_absolute(directory)
	var positions := PackedVector3Array()
	var uvs := PackedVector2Array()
	for i in 9000: positions.append(Vector3(float(i), -2.25, 0.0)); uvs.append(Vector2(NAN if i == 1 else 0.0, 0.125))
	var value := {"prototypes": [{"surfaces": [{"positions": positions, "uvs": uvs, "indices": PackedInt32Array([0,1,2]),
		"colors": PackedInt32Array([-1,0,16777215]), "empty": PackedByteArray()}]}],
		"meshes": [{"id": "/mesh_材🌲", "prototype": 0}], "empty": [], "object": {}, "bool": true, "null": null}
	roundtrip({"empty": []}, 1)
	roundtrip({"x": 1}, 7) # Exactly one full frame must still have a terminator.
	roundtrip({"text": "a\"\\\n\\u0000材🌲", "values": [true, false, null, -2.5]}, 3)
	roundtrip(value, 8192)
	roundtrip(value, 7)
	roundtrip({"tail": PackedByteArray([1, 2, 3]), "empty": PackedInt32Array()}, 3)
	var canceled_file := FileAccess.open(directory.path_join("canceled.bin"), FileAccess.WRITE)
	check(not Stream.new().write(canceled_file, value, 32, func(): return true), "Cancellation must stop a partial upload")
	check(canceled_file.get_length() == 0, "A canceled upload must not emit an empty complete message")
	var doc: Object = ClassDB.instantiate("RemixDocument")
	var project := directory.path_join("scène_材/work.usda")
	var created: Dictionary = doc.execute_data(JSON.stringify({"op": "create", "capture": ProjectSettings.globalize_path("res://../tests/fixtures/capture.usda"), "path": project}))
	check(created.get("ok", false) and created.result.document.path == project, "Direct bridge must preserve Unicode paths")
	var response: Dictionary = doc.execute_data('{"op":"scene"}')
	check(response.get("ok", false) and response.result.meshes.size() == 2 and response.result.prototypes.size() == 1, "Direct scene result must contain all meshes and share their geometry")
	var surface: Dictionary = response.result.prototypes[0].surfaces[0]
	check(surface.positions is PackedVector3Array and surface.normals is PackedVector3Array and surface.uvs is PackedVector2Array and surface.indices is PackedInt32Array,
		"Direct scene geometry must arrive as packed arrays")
	var text_response = JSON.parse_string(doc.execute('{"op":"scene"}'))
	check(text_response.result.prototypes[0].surfaces[0].positions.type == "vec3" and Marshalls.base64_to_raw(text_response.result.prototypes[0].surfaces[0].positions.base64) == surface.positions.to_byte_array(),
		"The text bridge must carry the same buffers as base64")
	check(not doc.execute_data('{"op":"invalid"}').get("ok", true), "Native exceptions must remain structured errors")

	# Exercise the UI's reported failure path without losing the USD document or
	# sending an empty scene after an unsuccessful native response.
	var app: Control = load("res://main.tscn").instantiate()
	app.sessions = Sessions.new(directory.path_join("recent.json"))
	root.add_child(app); app.render_toggle.button_pressed = false
	app._open_project(project)
	var actual_document: Object = app.document
	app.document = FailedScene.new()
	app._refresh(true)
	check(app.last_scene.is_empty() and app.viewport.scene.is_empty() and app.renderer.process.is_empty(), "Failed scene load must clear stale viewport state and stop rendering")
	check(app.state.get("open", false) and app.output.get_parsed_text().contains("Simulated scene failure"), "Failed scene load must retain the project and report the actual error")
	app.document = actual_document
	app._refresh(true)
	check(app.last_scene.meshes.size() == 2, "Scene loading must recover after a failed response")
	app.queue_free(); await process_frame

	# Optional read-only regression against a real large project. The worker
	# parses the entire streamed scene without needing to allocate GPU geometry.
	for arg in OS.get_cmdline_user_args():
		if not arg.begins_with("--project="): continue
		var path := arg.trim_prefix("--project=")
		check(doc.execute_data(JSON.stringify({"op": "open", "path": path})).get("ok", false), "Large project must open")
		print("Loading large scene: ", path)
		var start := Time.get_ticks_msec()
		response = doc.execute_data('{"op":"scene"}')
		check(response.get("ok", false), "Large scene must cross the native bridge")
		if not response.get("ok", false): continue
		var scene: Dictionary = response.result
		print("Loaded ", scene.meshes.size(), " meshes, ", scene.prototypes.size(), " prototypes in ", (Time.get_ticks_msec() - start) / 1000.0, "s")
		start = Time.get_ticks_msec()
		var geometry := Geometry.new()
		geometry.rebuild(scene)
		print("Layout geometry in ", (Time.get_ticks_msec() - start) / 1000.0, "s")
		check(geometry.entries.size() == scene.meshes.size(), "Large scene must build viewport geometry")
		print("Built layout geometry: ", geometry.entries.size(), " instances")
		var client := Client.new(); root.add_child(client); client.set_process(false)
		client.process = OS.execute_with_pipe(ProjectSettings.globalize_path("res://../build/remix-renderer"), [], true)
		var replies: Array = []
		client.response_received.connect(func(reply: Dictionary): replies.append(reply))
		client.writer = Thread.new(); client.writer.start(client._exchange_loop.bind(client.generation))
		start = Time.get_ticks_msec()
		client.send({"op": "large_scene_transport_probe", "scene": scene})
		client.send({"op": "hello"})
		var deadline := Time.get_ticks_msec() + 600000
		while replies.size() < 2 and Time.get_ticks_msec() < deadline: await process_frame
		check(replies.size() == 2, "Large scene upload and following message must both complete")
		if replies.size() == 2:
			check(replies[0].get("error", "") == "Unknown renderer operation: large_scene_transport_probe" and replies[1].get("ok", false), "Worker must parse the complete large scene and remain aligned")
		client.stop_worker(); client.queue_free()
		print("Large scene upload complete in ", (Time.get_ticks_msec() - start) / 1000.0, "s")
	print("Large scene bridge and streaming: %d failures" % failures)
	quit(1 if failures else 0)
