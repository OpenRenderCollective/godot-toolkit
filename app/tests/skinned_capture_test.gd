extends SceneTree

const LayoutGeometry = preload("res://scripts/layout_geometry.gd")
var failures := 0

func check(ok: bool, message: String) -> void:
	if not ok: push_error(message); failures += 1

func _initialize() -> void:
	var document: Object = ClassDB.instantiate("RemixDocument")
	var directory := ProjectSettings.globalize_path("res://../.work/skinning-ui/%d" % Time.get_ticks_usec()).simplify_path()
	var result: Dictionary = JSON.parse_string(document.execute(JSON.stringify({
		"op": "create", "capture": ProjectSettings.globalize_path("res://../tests/fixtures/skinned/capture.usda"),
		"path": directory.path_join("work.usda")})))
	check(result.get("ok", false), "Skinned capture project must load")
	var response: Dictionary = document.execute_data(JSON.stringify({"op": "scene"}))
	check(response.get("ok", false), "Skeleton scene must compose")
	var scene: Dictionary = response.get("result", {})
	check(scene.get("warnings", []).is_empty(), "Skeleton scene must not report composition warnings")
	var geometry := LayoutGeometry.new()
	geometry.rebuild(scene)
	check(geometry.entries.size() == 2, "Both skinned instances must have viewport geometry")
	if geometry.entries.size() == 2:
		check(geometry.entries[0].geometry != geometry.entries[1].geometry, "Different poses must have independent geometry and picking data")
		var posed := "/RootNode/instances/inst_0123456789ABCDEF_0/mesh"
		var rest := "/RootNode/instances/inst_0123456789ABCDEF_1/mesh"
		check(geometry.pick(Vector3(101.2, 0.2, 10), Vector3.FORWARD) == posed, "Picking must hit the captured pose")
		check(geometry.pick(Vector3(2.8, 21.2, 10), Vector3.FORWARD) == rest, "Unanimated instance must use the skeleton rest pose")
		check(geometry.pick(Vector3(112.8, 0.2, 10), Vector3.FORWARD).is_empty(), "Bind-pose geometry must not remain pickable")
		for entry in geometry.entries:
			if entry.id == posed:
				# ArrayMesh pads flat bounds slightly to keep its AABB usable.
				check(entry.bounds.position.distance_to(Vector3(101, 0, 4)) < 0.001 and entry.bounds.size.distance_to(Vector3(1, 1, 0)) < 0.001, "Framing bounds must follow the skinned pose: %s" % entry.bounds)
	print("Skinned capture viewport: %d failures" % failures)
	quit(1 if failures else 0)
