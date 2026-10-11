# SPDX-License-Identifier: LGPL-2.1-or-later
extends Node3D
# Renders a fixed number of frames, prints the frame rate, saves a screenshot and quits.
# User args (after "--"): --frames=N  --out=<png path>

var frames_target := 120
var out_path := ""
var frame := 0
var t0 := 0

func _ready() -> void:
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--frames="):
			frames_target = int(a.substr(9))
		elif a.begins_with("--out="):
			out_path = a.substr(6)
	# A texture generated at runtime: a checkerboard on the sphere.
	var img := Image.create(64, 64, false, Image.FORMAT_RGBA8)
	for y in 64:
		for x in 64:
			var c := Color(0.95, 0.85, 0.1) if ((x / 8 + y / 8) % 2 == 0) else Color(0.1, 0.2, 0.7)
			img.set_pixel(x, y, c)
	img.generate_mipmaps()
	var mat := StandardMaterial3D.new()
	mat.albedo_texture = ImageTexture.create_from_image(img)
	$Sphere.set_surface_override_material(0, mat)
	print("RENDERER: ", RenderingServer.get_video_adapter_name(), " | ", RenderingServer.get_current_rendering_driver_name(), " | ", RenderingServer.get_current_rendering_method())
	t0 = Time.get_ticks_usec()

func _process(delta: float) -> void:
	frame += 1
	$Box.rotate_y(delta)
	if frame == 5:
		t0 = Time.get_ticks_usec()
	if frame >= frames_target:
		var secs := (Time.get_ticks_usec() - t0) / 1e6
		print("FPS: ", (frame - 5) / secs, " frames=", frame - 5)
		if out_path != "":
			var im := get_viewport().get_texture().get_image()
			var err := im.save_png(out_path)
			print("SCREENSHOT: ", out_path, " err=", err, " size=", im.get_size())
		get_tree().quit()
