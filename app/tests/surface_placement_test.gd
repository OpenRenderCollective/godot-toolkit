extends SceneTree

const LayoutView = preload("res://scripts/layout_view.gd")
const Gizmo = preload("res://scripts/transform_gizmo.gd")
var failures := 0

func check(condition: bool, message: String) -> void:
	if not condition: push_error(message); failures += 1

func _initialize() -> void:
	run.call_deferred()

func run() -> void:
	var view := LayoutView.new(); root.add_child(view); view.size = Vector2(800, 600)
	var surface := {"positions": PackedVector3Array([Vector3(-10,-10,0), Vector3(10,-10,0), Vector3(10,10,0), Vector3(-10,10,0)]), "indices": PackedInt32Array([0,1,2,0,2,3])}
	for up in [Vector3.UP, Vector3.BACK]:
		var rotation := Basis(Vector3.RIGHT, -PI / 2) if up == Vector3.UP else Basis.IDENTITY
		view.set_scene({"up_axis": "Y" if up == Vector3.UP else "Z", "meters_per_unit": 1,
			"prototypes": [{"surfaces": [surface]}], "meshes": [{"id": "/Ground", "transform": Gizmo.to_array(Transform3D(rotation, up * 2)), "prototype": 0}]})
		view.pitch = 0.9; view.yaw = 0.2; view.center = -up * 10; view.distance = 40
		view.rendered_texture = ImageTexture.create_from_image(Image.create(960, 540, false, Image.FORMAT_RGBA8))
		view.show_render = true
		# Aim at an elevated ground mesh while the camera pivot lies below it.
		var screen := view._project(up * 2)
		var hit := view.surface_at(screen)
		check(not hit.is_empty() and hit.path == "/Ground" and hit.position.is_equal_approx(up * 2), "Placement must use actual visible depth in either up axis")
		var pose := Transform3D(Basis.from_scale(Vector3(2, 1, 1)), Vector3(1, 2, 3))
		var details := {"path": "/Light", "authoring": {"kind": "light", "type": "SphereLight"}, "properties": [{"name": "inputs:radius", "value": 0.5}]}
		var placed := view.placement_transform(pose, hit, details)
		check(placed.origin.is_equal_approx(up * 3.01) and placed.basis.is_equal_approx(pose.basis), "Placement must clear the emitter radius and preserve rotation/scale")
		check(view.surface_at(Vector2.ZERO).is_empty(), "Letterbox bars are not scene placement targets")
		view.center = up * 2; view.distance = 20
		var spawn := view.light_spawn_transform("SpotLight", {"inputs:radius": 5})
		check(spawn.origin.is_equal_approx(up * 7.01), "New spotlights must clear the actual floor by their emitter radius")
		check(spawn.basis.z.is_equal_approx(up), "New directional emitters must point toward the hit surface")
		var prop := Transform3D(Basis.from_scale(Vector3(1, 2, 3)), up * 30)
		var data: Dictionary = view.scene.duplicate(true)
		data.meshes.append({"id": "/Prop", "transform": Gizmo.to_array(prop), "prototype": 0})
		view.set_scene(data)
		var mesh_placement := view.placement_transform(prop, hit, {"path": "/Prop", "authoring": {"kind": "mesh"}})
		var bottom := 20.0 if up == Vector3.UP else 0.0
		check(mesh_placement.origin.is_equal_approx(up * (2.01 + bottom)) and mesh_placement.basis.is_equal_approx(prop.basis),
			"Meshes must rest on the hit surface using their bounds, preserving scale")
		data.meshes.pop_back(); view.set_scene(data)
		view.center = up * 20; view.pitch = -0.5
		check(view.surface_at(view._view_rect().get_center()).is_empty(), "Looking into the sky must not invent a surface depth")
		check(view.light_spawn_transform("SphereLight", {"inputs:radius": 5}).origin.is_equal_approx(view.center), "Spawning in empty sky retains a usable focus-distance fallback")
	view.queue_free(); await process_frame
	print("Surface placement: %d failures" % failures)
	quit(1 if failures else 0)
