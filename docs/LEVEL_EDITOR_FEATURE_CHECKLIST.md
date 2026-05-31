# Real-Time Path-Traced Level Editor Feature Checklist

This checklist defines the features needed to turn the current renderer/editor into a complete level editor similar to the provided references.

## Core Rule: Separate Scene Loading From Asset Importing

The editor must clearly separate these operations:

```text
Open Scene        = replace the current scene with a saved .rtlevel / .mscene
Import Asset      = add reusable asset(s) into Content Browser only
Import and Place  = import asset(s), then create instance(s) in the current scene
Import New Scene  = create/replace the current scene from an external file
Merge Scene       = add external scene contents into the current scene
```

Current issue:

```text
Import glTF -> creates a whole other scene
```

Correct behavior:

```text
Import glTF -> creates mesh/material/texture/prefab assets
Drag prefab/model into viewport -> creates entities in the current scene
```

---

# 1. Top Menu Bar

Target menu layout:

```text
File | Create | Engine | Window | Render | Layout
```

---

# 2. File Menu

```text
File
 ├─ New Project...
 ├─ Open Project...
 ├─ Recent Projects
 ├─ Close Project
 │
 ├─ New Scene
 ├─ Open Scene...
 ├─ Recent Scenes
 ├─ Close Scene
 │
 ├─ Save Scene
 ├─ Save Scene As...
 ├─ Save All
 ├─ Autosave Now
 │
 ├─ Import
 │   ├─ Import Asset...
 │   ├─ Import Model as Asset...
 │   ├─ Import and Place...
 │   ├─ Import Scene as New Scene...
 │   ├─ Merge Scene into Current...
 │   ├─ Import Texture...
 │   ├─ Import HDRI...
 │   ├─ Import VDB...
 │   ├─ Import IES Profile...
 │   └─ Reimport Selected
 │
 ├─ Export
 │   ├─ Export Scene...
 │   ├─ Export Selected...
 │   ├─ Export Screenshot...
 │   ├─ Export High Resolution Render...
 │   └─ Export Debug Buffers...
 │
 ├─ Project Settings...
 ├─ Editor Preferences...
 └─ Exit
```

Important shortcuts:

```text
Ctrl+N        New Scene
Ctrl+O        Open Scene
Ctrl+S        Save Scene
Ctrl+Shift+S  Save Scene As
Ctrl+I        Import Asset
Alt+F4        Exit
```

---

# 3. Create Menu

```text
Create
 ├─ Empty Entity
 ├─ Folder / Group
 │
 ├─ 3D Object
 │   ├─ Cube
 │   ├─ Sphere
 │   ├─ Plane
 │   ├─ Cylinder
 │   ├─ Cone
 │   ├─ Grid
 │   └─ Mesh From Asset...
 │
 ├─ Light
 │   ├─ Directional Light / Sun
 │   ├─ Point Light
 │   ├─ Spot Light
 │   ├─ Rect Area Light
 │   ├─ Disk Area Light
 │   ├─ Sphere Light
 │   └─ Emissive Mesh Light
 │
 ├─ Camera
 │   ├─ Camera
 │   └─ Cine Camera
 │
 ├─ Environment
 │   ├─ Environment Light
 │   ├─ Sky Atmosphere
 │   ├─ Height Fog
 │   ├─ Volumetric Cloud
 │   ├─ Post Process Volume
 │   └─ Reflection Capture / Radiance Cache Probe
 │
 ├─ Volume
 │   ├─ Box Volume
 │   ├─ Sphere Volume
 │   ├─ VDB Volume
 │   └─ Participating Media Volume
 │
 ├─ Material
 │   ├─ Standard PBR Material
 │   ├─ Glass Material
 │   ├─ Emissive Material
 │   ├─ Volume Material
 │   └─ Material Instance
 │
 ├─ Physics
 │   ├─ Static Collider
 │   ├─ Box Collider
 │   ├─ Sphere Collider
 │   ├─ Capsule Collider
 │   └─ Rigid Body
 │
 └─ Prefab
     ├─ Create Prefab From Selection
     └─ Place Prefab...
```

Minimum first implementation:

```text
Empty Entity
Folder / Group
Mesh From Asset
Sun Light
Point Light
Spot Light
Area Light
Camera
Environment Light
Sky Atmosphere
Height Fog
Post Process Volume
VDB / Volume
Standard PBR Material
Create Prefab From Selection
```

---

# 4. Engine Menu

```text
Engine
 ├─ Play
 ├─ Simulate
 ├─ Pause
 ├─ Stop
 │
 ├─ Build / Cook
 │   ├─ Cook Assets
 │   ├─ Build Standalone Viewer
 │   ├─ Build Game
 │   └─ Package Project
 │
 ├─ Asset Pipeline
 │   ├─ Reimport Changed Assets
 │   ├─ Rebuild Asset Database
 │   ├─ Rebuild Thumbnails
 │   ├─ Validate Asset References
 │   ├─ Find Missing Assets
 │   └─ Clear Asset Cache
 │
 ├─ Renderer Resources
 │   ├─ Rebuild BLAS
 │   ├─ Rebuild TLAS
 │   ├─ Rebuild Environment CDF
 │   ├─ Rebuild Light Sampling Table
 │   ├─ Rebuild Atmosphere LUTs
 │   ├─ Clear GPU Caches
 │   └─ Reset Renderer State
 │
 ├─ Shader Tools
 │   ├─ Recompile Shaders
 │   ├─ Reload Shaders
 │   ├─ Open Shader Cache Folder
 │   └─ Clear Shader Cache
 │
 ├─ Diagnostics
 │   ├─ GPU Profiler
 │   ├─ CPU Profiler
 │   ├─ Memory Report
 │   ├─ Resource Lifetime Report
 │   ├─ Frame Timeline Dump
 │   ├─ Render Graph Viewer
 │   ├─ Image Diff Tool
 │   ├─ Baseline Regression Check
 │   └─ Crash Report Folder
 │
 └─ Engine Settings...
```

---

# 5. Window Menu

```text
Window
 ├─ Viewport
 ├─ Scene Hierarchy
 ├─ Inspector / Properties
 ├─ Render Settings
 ├─ Render World Settings
 ├─ Content Browser
 ├─ Asset Browser
 ├─ Material Editor
 ├─ Texture Viewer
 ├─ Mesh Viewer
 ├─ Prefab Editor
 ├─ Timeline
 ├─ Sequencer
 ├─ Log
 ├─ Console
 ├─ Debug / Profiler
 ├─ GPU Profiler
 ├─ CPU Profiler
 ├─ Render Graph Viewer
 ├─ Statistics
 ├─ Camera Preview
 ├─ Environment Editor
 ├─ Atmosphere Editor
 ├─ Denoiser Debugger
 ├─ ReSTIR Debugger
 ├─ Memory Viewer
 └─ Asset Import Queue
```

Minimum first implementation:

```text
Viewport
Scene Hierarchy
Inspector
Content Browser
Render Settings
Render World Settings
Material Editor
Timeline
Log
Console
Profiler
```

---

# 6. Render Menu

```text
Render
 ├─ Render Mode
 │   ├─ Path Traced
 │   ├─ Denoised Path Traced
 │   ├─ Raster Preview
 │   ├─ Hybrid
 │   └─ Reference Offline
 │
 ├─ View Mode
 │   ├─ Beauty
 │   ├─ Albedo
 │   ├─ Normal
 │   ├─ Depth
 │   ├─ World Position
 │   ├─ Roughness
 │   ├─ Metallic
 │   ├─ Emissive
 │   ├─ Opacity
 │   ├─ Motion Vectors
 │   ├─ Object ID
 │   ├─ Material ID
 │   ├─ Instance ID
 │   ├─ Direct Lighting
 │   ├─ Indirect Lighting
 │   ├─ Diffuse
 │   ├─ Specular
 │   ├─ Transmission
 │   ├─ Volume Only
 │   ├─ Atmosphere Only
 │   ├─ Shadow Mask
 │   ├─ Reprojection Confidence
 │   ├─ History Length
 │   ├─ Accumulation Count
 │   ├─ Variance
 │   ├─ Firefly Clamp Debug
 │   └─ NaN / Inf Debug
 │
 ├─ Quality Preset
 │   ├─ Interactive
 │   ├─ Preview
 │   ├─ Beauty
 │   ├─ Final
 │   └─ Custom
 │
 ├─ Denoiser
 │   ├─ Enable Denoiser
 │   ├─ Enable TAA
 │   ├─ Enable Reprojection
 │   ├─ Reset History
 │   ├─ Lock History
 │   └─ Denoiser Settings...
 │
 ├─ ReSTIR
 │   ├─ Enable ReSTIR DI
 │   ├─ Enable ReSTIR GI
 │   ├─ Enable ReSTIR PT
 │   └─ ReSTIR Settings...
 │
 ├─ Accumulation
 │   ├─ Pause Accumulation
 │   ├─ Reset Accumulation
 │   ├─ Lock Camera Jitter
 │   ├─ Set Target Samples...
 │   └─ Clear History
 │
 ├─ Screenshot / Capture
 │   ├─ Capture Viewport
 │   ├─ Capture High Resolution
 │   ├─ Capture Debug View
 │   ├─ Capture All AOVs
 │   └─ Start/Stop Video Capture
 │
 └─ Render Settings...
```

---

# 7. Layout Menu

```text
Layout
 ├─ Save Layout
 ├─ Load Layout
 ├─ Reset Layout
 ├─ Lock Layout
 │
 ├─ Workspaces
 │   ├─ Default
 │   ├─ Level Editing
 │   ├─ Lighting
 │   ├─ Rendering
 │   ├─ Material Editing
 │   ├─ Animation
 │   ├─ Debugging
 │   └─ Minimal
 │
 ├─ Panels
 │   ├─ Show All Panels
 │   ├─ Hide Side Panels
 │   ├─ Hide Bottom Panel
 │   ├─ Focus Viewport
 │   └─ Fullscreen Viewport
 │
 ├─ UI Scale
 │   ├─ 75%
 │   ├─ 100%
 │   ├─ 125%
 │   ├─ 150%
 │   └─ 200%
 │
 └─ Theme
     ├─ Dark
     ├─ Light
     └─ High Contrast
```

---

# 8. Import System

## Import Modes

```text
Import as Asset
 └─ Adds reusable assets to the Content Browser only.

Import and Place
 └─ Adds reusable assets to the Content Browser, then places an instance in the current scene.

Import Scene as New Scene
 └─ Creates or replaces the current scene from an external scene/model file.

Merge Scene into Current
 └─ Adds imported scene hierarchy into the currently open scene.

Reimport Selected
 └─ Updates an existing imported asset while preserving scene references.
```

## Import Dialog

```text
Import Model

Source:
C:/Assets/car.glb

Destination:
Content/Models/car/

Import Mode:
[ ] Import as Asset
[ ] Import and Place in Current Scene
[ ] Import as New Scene
[ ] Merge into Current Scene

Geometry:
[x] Import meshes
[x] Generate tangents
[x] Merge primitives by material
[ ] Combine meshes
[x] Preserve node hierarchy
[ ] Flatten hierarchy

Materials:
[x] Import materials
[x] Import textures
[x] Detect normal maps
[x] Detect roughness/metallic maps
[x] Convert textures to engine format

Acceleration:
[x] Build BLAS cache
[x] Generate bounds
[x] Generate thumbnails
```

## File Format Support

```text
Phase 1 — Must-have
 ├─ .gltf
 ├─ .glb
 ├─ .obj
 ├─ .mtl
 ├─ .png
 ├─ .jpg / .jpeg
 ├─ .hdr
 └─ .exr

Phase 2 — Important
 ├─ .fbx
 ├─ .tga
 ├─ .dds
 ├─ .ktx2
 ├─ .ies
 └─ .vdb

Phase 3 — Later
 ├─ .usd / .usdz
 ├─ .abc
 ├─ .materialx
 └─ .blend
```

## glTF / GLB Support Checklist

```text
Geometry
 ├─ Nodes
 ├─ Meshes
 ├─ Primitives
 ├─ Indices
 ├─ Positions
 ├─ Normals
 ├─ Tangents
 ├─ UV0
 ├─ UV1
 ├─ Vertex colors
 ├─ Skinning later
 └─ Morph targets later

Materials
 ├─ Base color
 ├─ Metallic
 ├─ Roughness
 ├─ Normal map
 ├─ Occlusion map
 ├─ Emissive map
 ├─ Alpha mode
 ├─ Alpha cutoff
 ├─ Double-sided
 ├─ Transmission extension
 ├─ IOR extension
 ├─ Clearcoat extension
 └─ Sheen extension

Textures
 ├─ Embedded GLB textures
 ├─ External glTF textures
 ├─ Texture path resolving
 ├─ Sampler settings
 ├─ sRGB / linear handling
 ├─ Mipmap generation
 └─ Missing texture fallback

Scene Data
 ├─ Node hierarchy
 ├─ Local transforms
 ├─ Multiple scenes
 ├─ Cameras
 ├─ KHR_lights_punctual
 ├─ Unit scale
 ├─ Coordinate conversion
 └─ Animations later
```

## OBJ Support Checklist

```text
OBJ Import
 ├─ Geometry
 ├─ Indices
 ├─ Normals
 ├─ UVs
 ├─ Material groups
 ├─ .mtl loading
 ├─ Diffuse texture map_Kd
 ├─ Normal map bump/map_Bump
 ├─ Roughness texture if available
 ├─ Metallic texture if available
 ├─ Generate normals if missing
 ├─ Generate tangents
 └─ Split mesh by material
```

## Import Output

A model import should generate:

```text
Imported Model Output
 ├─ Mesh assets
 ├─ Material assets
 ├─ Texture assets
 ├─ Optional animation assets
 ├─ Optional light/camera assets
 ├─ Prefab asset
 └─ Optional scene asset
```

Example:

```text
Content/Models/Car/
 ├─ Car.prefab
 ├─ Meshes/
 │   ├─ Body.mesh
 │   └─ Wheel.mesh
 ├─ Materials/
 │   ├─ Paint.material
 │   └─ Glass.material
 └─ Textures/
     ├─ Paint_BaseColor.texture
     ├─ Paint_Normal.texture
     └─ Paint_Roughness.texture
```

---

# 9. Asset Database

```text
Asset Database
 ├─ Asset GUID
 ├─ Asset type
 ├─ Source path
 ├─ Imported/cache path
 ├─ Thumbnail path
 ├─ Import settings
 ├─ Dependencies
 ├─ References
 ├─ Last modified timestamp
 ├─ Missing asset detection
 ├─ Reimport tracking
 ├─ Asset registry file
 └─ Asset validation
```

## Asset Types

```text
Asset Types
 ├─ Mesh
 ├─ Material
 ├─ Texture
 ├─ HDRI
 ├─ Scene
 ├─ Prefab
 ├─ VDB Volume
 ├─ IES Profile
 ├─ Animation
 ├─ Script
 └─ Shader
```

---

# 10. Content Browser

```text
Content Browser
 ├─ Add / Import button
 ├─ Search bar
 ├─ Breadcrumb path
 ├─ Back / forward buttons
 ├─ Refresh button
 │
 ├─ Folder tree
 │   ├─ Content
 │   ├─ Models
 │   ├─ Materials
 │   ├─ Textures
 │   ├─ HDRI
 │   ├─ Scenes
 │   ├─ Prefabs
 │   ├─ VDB
 │   └─ Scripts
 │
 ├─ Asset view
 │   ├─ Grid view
 │   ├─ List view
 │   ├─ Thumbnails
 │   ├─ File type icons
 │   ├─ Sorting
 │   └─ Filtering
 │
 ├─ Asset operations
 │   ├─ Open
 │   ├─ Rename
 │   ├─ Duplicate
 │   ├─ Delete
 │   ├─ Import
 │   ├─ Reimport
 │   ├─ Show in Explorer
 │   ├─ Copy path
 │   ├─ Show dependencies
 │   └─ Find references
 │
 └─ Drag-drop
     ├─ Drag mesh to viewport
     ├─ Drag prefab to viewport
     ├─ Drag material to mesh
     ├─ Drag HDRI to environment
     ├─ Drag texture to material slot
     └─ Drag VDB to volume
```

---

# 11. Scene / Level System

```text
Scene System
 ├─ .rtlevel / .mscene file
 ├─ Scene name
 ├─ Scene GUID
 ├─ Entity list
 ├─ Component data
 ├─ Parent-child hierarchy
 ├─ Asset references by GUID
 ├─ World settings
 ├─ Render settings
 ├─ Active camera
 ├─ Startup camera
 ├─ Scene dirty state
 ├─ Autosave
 ├─ Recovery file
 ├─ Save scene
 ├─ Save scene as
 ├─ Load scene
 ├─ Merge scene
 └─ Validate scene
```

---

# 12. Entity / Component System

## Required Components

```text
Required Components
 ├─ Transform
 ├─ Mesh Renderer
 ├─ Material Slot
 ├─ Camera
 ├─ Light
 ├─ Environment Light
 ├─ Sky Atmosphere
 ├─ Height Fog
 ├─ Volumetric Cloud
 ├─ Volume Renderer
 ├─ Post Process Volume
 ├─ Animation
 ├─ Script
 ├─ Audio Source
 ├─ Collider
 ├─ Rigid Body
 └─ Custom Component
```

## Minimum First Version

```text
Minimum Components
 ├─ Transform
 ├─ Mesh Renderer
 ├─ Material Slot
 ├─ Camera
 ├─ Light
 ├─ Environment Light
 ├─ Sky Atmosphere
 ├─ Fog
 ├─ Volume
 └─ Post Process
```

---

# 13. Scene Hierarchy / Outliner

```text
Scene Hierarchy
 ├─ Search / filter bar
 ├─ Scene root
 ├─ Entity rows
 │   ├─ Expand / collapse arrow
 │   ├─ Type icon
 │   ├─ Entity name
 │   ├─ Visibility eye
 │   ├─ Lock icon
 │   └─ Selection highlight
 │
 ├─ Parent-child hierarchy
 ├─ Drag-drop reparenting
 ├─ Multi-selection
 ├─ Rename entity
 ├─ Duplicate entity
 ├─ Delete entity
 ├─ Group / ungroup
 ├─ Create child entity
 ├─ Hide / show entity
 ├─ Lock / unlock entity
 ├─ Solo / isolate entity
 ├─ Focus selected
 └─ Convert selection to prefab
```

## Entity Type Icons

```text
Entity Type Icons
 ├─ Empty entity
 ├─ Folder / group
 ├─ Mesh
 ├─ Camera
 ├─ Directional light
 ├─ Point light
 ├─ Spot light
 ├─ Area light
 ├─ Environment light
 ├─ Sky atmosphere
 ├─ Fog
 ├─ Volume
 ├─ Post-process volume
 ├─ Prefab
 └─ Missing / broken entity
```

---

# 14. Inspector / Properties

```text
Inspector
 ├─ Entity name
 ├─ Entity enabled checkbox
 ├─ Static / dynamic flag
 ├─ Layer / tag
 │
 ├─ Transform Component
 │   ├─ Position
 │   ├─ Rotation
 │   ├─ Scale
 │   ├─ Reset position
 │   ├─ Reset rotation
 │   ├─ Reset scale
 │   ├─ Copy transform
 │   └─ Paste transform
 │
 ├─ Mesh Renderer Component
 │   ├─ Visible
 │   ├─ Cast shadows
 │   ├─ Visible to camera
 │   ├─ Visible to reflections
 │   ├─ Mesh asset
 │   ├─ Material slots
 │   └─ Render flags
 │
 ├─ Material Component
 │   ├─ Base color
 │   ├─ Metallic
 │   ├─ Roughness
 │   ├─ Specular
 │   ├─ Transmission
 │   ├─ IOR
 │   ├─ Emissive color
 │   ├─ Emissive strength
 │   ├─ Opacity
 │   ├─ Alpha mode
 │   ├─ Double-sided
 │   └─ Texture slots
 │
 ├─ Light Component
 │   ├─ Light type
 │   ├─ Intensity
 │   ├─ Units
 │   ├─ Color
 │   ├─ Color temperature
 │   ├─ Radius / size
 │   ├─ Cone angle
 │   ├─ Penumbra
 │   ├─ Cast shadows
 │   ├─ Cast volumetric shadows
 │   └─ IES profile
 │
 ├─ Camera Component
 │   ├─ Projection
 │   ├─ FOV
 │   ├─ Focal length
 │   ├─ Sensor size / film size
 │   ├─ Near clip
 │   ├─ Far clip
 │   ├─ Exposure mode
 │   ├─ ISO
 │   ├─ Shutter speed
 │   ├─ Aperture
 │   ├─ DOF
 │   ├─ Focus distance
 │   └─ Motion blur
 │
 ├─ Environment Component
 │   ├─ HDRI asset
 │   ├─ Intensity
 │   ├─ Rotation
 │   ├─ Visible to camera
 │   └─ Importance sampling rebuild
 │
 ├─ Sky Atmosphere Component
 │   ├─ Planet radius
 │   ├─ Atmosphere height
 │   ├─ Rayleigh scattering
 │   ├─ Mie scattering
 │   ├─ Ozone absorption
 │   ├─ Multi-scattering
 │   ├─ Ground albedo
 │   └─ LUT settings
 │
 ├─ Fog Component
 │   ├─ Density
 │   ├─ Height falloff
 │   ├─ Color
 │   ├─ Anisotropy
 │   └─ Max distance
 │
 ├─ Volume Component
 │   ├─ Volume asset
 │   ├─ Density scale
 │   ├─ Emission scale
 │   ├─ Scattering color
 │   ├─ Absorption color
 │   └─ Step count
 │
 ├─ Post Process Component
 │   ├─ Exposure
 │   ├─ Tone mapping
 │   ├─ Bloom
 │   ├─ Color grading
 │   ├─ Vignette
 │   ├─ Film grain
 │   └─ Depth of field
 │
 └─ Add Component Button
```

---

# 15. Viewport

## Viewport Toolbar

```text
Viewport Toolbar
 ├─ Select Tool
 ├─ Move Tool
 ├─ Rotate Tool
 ├─ Scale Tool
 ├─ Universal Transform Tool
 ├─ Local / World Space Toggle
 ├─ Pivot / Center Toggle
 ├─ Snap Toggle
 ├─ Grid Toggle
 ├─ View Settings
 ├─ Stats
 ├─ Draw Debug
 ├─ Camera Speed
 ├─ View Mode
 └─ Render Quality Mode
```

## Required Gizmos

```text
Required Gizmos
 ├─ Move gizmo
 │   ├─ X / Y / Z arrows
 │   ├─ XY / YZ / XZ plane handles
 │   └─ Center handle
 │
 ├─ Rotate gizmo
 │   ├─ X / Y / Z rings
 │   └─ Screen-space rotation ring
 │
 ├─ Scale gizmo
 │   ├─ X / Y / Z handles
 │   └─ Uniform scale handle
 │
 ├─ Light gizmos
 │   ├─ Point light radius
 │   ├─ Spot light cone
 │   ├─ Area light rectangle / disk
 │   └─ Sun direction arrow
 │
 ├─ Camera gizmos
 │   ├─ Frustum
 │   ├─ Near / far plane
 │   └─ Camera preview
 │
 └─ Volume gizmos
     ├─ Box volume bounds
     ├─ Sphere volume bounds
     └─ VDB bounds
```

## Viewport Overlays

```text
Viewport Overlays
 ├─ FPS / frame time
 ├─ GPU time
 ├─ CPU time
 ├─ Samples
 ├─ Resolution scale
 ├─ Active view mode
 ├─ Denoiser status
 ├─ TAA status
 ├─ ReSTIR status
 ├─ Selected entity name
 ├─ Reset reason
 ├─ Axis orientation widget
 ├─ Grid
 ├─ Camera safe frame
 ├─ Object outline
 ├─ Bounding boxes
 ├─ Light icons
 ├─ Camera icons
 ├─ Volume bounds
 └─ Debug text panel
```

---

# 16. Editing Commands

```text
Editor Commands
 ├─ Select
 ├─ Multi-select
 ├─ Move
 ├─ Rotate
 ├─ Scale
 ├─ Duplicate
 ├─ Delete
 ├─ Rename
 ├─ Parent
 ├─ Unparent
 ├─ Group
 ├─ Ungroup
 ├─ Create entity
 ├─ Create component
 ├─ Remove component
 ├─ Change property
 ├─ Assign material
 ├─ Replace mesh
 ├─ Import asset
 ├─ Place asset
 ├─ Reimport asset
 ├─ Save scene
 └─ Load scene
```

## Undo / Redo

```text
Undo / Redo System
 ├─ Ctrl+Z undo
 ├─ Ctrl+Y redo
 ├─ Command stack
 ├─ Transaction grouping
 ├─ Transform edit transaction
 ├─ Property edit transaction
 ├─ Create / delete transaction
 ├─ Material assignment transaction
 ├─ Asset placement transaction
 └─ Scene dirty integration
```

---

# 17. Keyboard Shortcuts

```text
Essential Shortcuts
 ├─ Ctrl+N        New Scene
 ├─ Ctrl+O        Open Scene
 ├─ Ctrl+S        Save Scene
 ├─ Ctrl+Shift+S  Save Scene As
 ├─ Ctrl+I        Import Asset
 ├─ Ctrl+Z        Undo
 ├─ Ctrl+Y        Redo
 ├─ Ctrl+D        Duplicate
 ├─ Delete        Delete Selected
 ├─ F             Frame Selected
 ├─ W             Move Tool
 ├─ E             Rotate Tool
 ├─ R             Scale Tool
 ├─ Q             Select Tool
 ├─ G             Toggle Grid
 ├─ Ctrl+G        Group
 ├─ Ctrl+Shift+G  Ungroup
 ├─ F1            Beauty View
 ├─ F2            Albedo View
 ├─ F3            Normal View
 ├─ F4            Depth View
 ├─ F5            Motion Vector View
 ├─ F6            Reprojection Confidence
 ├─ F7            Object ID
 └─ F8            Material ID
```

---

# 18. Render World Settings

These should be separated from technical renderer/debug controls.

```text
Render World Settings
 ├─ Environment
 │   ├─ HDRI
 │   ├─ Intensity
 │   ├─ Rotation
 │   └─ Visibility
 │
 ├─ Sun / Sky
 │   ├─ Sun direction
 │   ├─ Sun intensity
 │   ├─ Sun color temperature
 │   ├─ Sun disk size
 │   └─ Atmosphere binding
 │
 ├─ Sky Atmosphere
 │   ├─ Rayleigh
 │   ├─ Mie
 │   ├─ Ozone
 │   ├─ Multi-scattering
 │   └─ LUT quality
 │
 ├─ Fog
 │   ├─ Density
 │   ├─ Height falloff
 │   └─ Color
 │
 ├─ Volumetric Clouds
 │   ├─ Cloud asset
 │   ├─ Coverage
 │   ├─ Density
 │   ├─ Wind
 │   └─ Shadowing
 │
 ├─ Exposure
 │   ├─ Manual
 │   ├─ Auto exposure
 │   ├─ EV
 │   ├─ ISO
 │   ├─ Shutter
 │   └─ Aperture
 │
 ├─ Post Process
 │   ├─ Tone mapping
 │   ├─ Bloom
 │   ├─ Vignette
 │   ├─ Color grading
 │   ├─ Film grain
 │   └─ Depth of field
 │
 └─ Global Illumination
     ├─ Path tracing
     ├─ ReSTIR
     ├─ Radiance cache
     └─ Denoiser
```

---

# 19. Technical Render Settings

```text
Render Settings
 ├─ Render preset
 ├─ View mode
 ├─ Resolution scale
 ├─ Samples per frame
 ├─ Max bounces
 ├─ Direct lighting
 ├─ Indirect lighting
 ├─ Environment lighting
 ├─ MIS
 ├─ ReSTIR DI
 ├─ ReSTIR GI
 ├─ ReSTIR PT
 ├─ TAA
 ├─ Denoiser
 ├─ Reprojection
 ├─ SER
 ├─ Opacity micromaps
 ├─ Accumulation
 ├─ Debug view
 ├─ GPU timings
 └─ Reset history
```

---

# 20. Debug Views

```text
Debug Views
 ├─ Beauty
 ├─ Albedo
 ├─ Normal
 ├─ Depth
 ├─ Linear depth
 ├─ World position
 ├─ Roughness
 ├─ Metallic
 ├─ Specular
 ├─ Emissive
 ├─ Opacity
 ├─ Motion vectors
 ├─ Object ID
 ├─ Material ID
 ├─ Instance ID
 ├─ Triangle ID
 ├─ Direct lighting
 ├─ Indirect lighting
 ├─ Diffuse lighting
 ├─ Specular lighting
 ├─ Environment contribution
 ├─ Sun contribution
 ├─ Light contribution
 ├─ Volume scattering
 ├─ Atmosphere
 ├─ Shadow mask
 ├─ Reprojection confidence
 ├─ History length
 ├─ Variance
 ├─ Adaptive alpha
 ├─ Disocclusion mask
 ├─ Reactive mask
 ├─ Firefly clamp
 ├─ Accumulation count
 ├─ Denoiser input
 ├─ Denoiser output
 ├─ NaN / Inf
 └─ GPU cost / heatmap
```

---

# 21. Timeline / Sequencer

```text
Timeline
 ├─ Play
 ├─ Pause
 ├─ Stop
 ├─ Frame number
 ├─ Start frame
 ├─ End frame
 ├─ Scrubber
 ├─ Keyframe markers
 ├─ Add keyframe
 ├─ Delete keyframe
 ├─ Transform tracks
 ├─ Camera tracks
 ├─ Light tracks
 ├─ Material tracks
 ├─ Sun / sky tracks
 ├─ Volume tracks
 └─ Export camera animation
```

Minimum:

```text
Minimum Timeline
 ├─ Play / pause
 ├─ Frame number
 ├─ Start / end frame
 ├─ Scrubber
 └─ Transform keyframes
```

---

# 22. Project Manager

```text
Project Manager
 ├─ New Project
 ├─ Open Project
 ├─ Recent Projects
 ├─ Project template
 ├─ Project location
 ├─ Startup scene
 ├─ Default render preset
 ├─ Content folder
 ├─ Cache folder
 ├─ Saved folder
 ├─ Config folder
 ├─ Build folder
 └─ Project settings
```

Recommended project structure:

```text
MyProject/
 ├─ MyProject.rtproject
 ├─ Content/
 ├─ Scenes/
 ├─ Cache/
 ├─ Saved/
 │   ├─ Autosaves/
 │   └─ Logs/
 ├─ Config/
 └─ Build/
```

---

# 23. Prefab System

```text
Prefab System
 ├─ Create prefab from selection
 ├─ Create prefab from imported model
 ├─ Place prefab
 ├─ Nested prefabs
 ├─ Prefab overrides
 ├─ Apply overrides
 ├─ Revert overrides
 ├─ Break prefab link
 ├─ Prefab thumbnail
 └─ Prefab asset references
```

Useful prefab examples:

```text
Tree
Rock
Lamp
Door
Car
Camera rig
Light setup
Fog volume
Cloud volume
Post-process volume
```

---

# 24. Autosave / Recovery

```text
Autosave System
 ├─ Autosave every N minutes
 ├─ Autosave after major edits
 ├─ Recovery prompt on startup
 ├─ Backup scene versions
 ├─ Unsaved scene marker
 ├─ Crash recovery
 └─ Manual recover from autosave
```

---

# 25. Cook / Build System

```text
Cook / Build
 ├─ Cook assets
 ├─ Build standalone viewer
 ├─ Build game
 ├─ Package project
 ├─ Copy used assets only
 ├─ Convert meshes to engine format
 ├─ Compress textures
 ├─ Build material cache
 ├─ Build BLAS cache
 ├─ Build environment CDF
 ├─ Build shader cache
 ├─ Validate references
 └─ Output build folder
```

Example output:

```text
Build/
 ├─ Game.exe
 ├─ Content.pak
 ├─ startup.rtlevel
 ├─ shaders.cache
 └─ config.json
```

---

# 26. Play Mode / Game Mode

```text
Play Mode
 ├─ Play in editor
 ├─ Simulate
 ├─ Stop
 ├─ Pause
 ├─ Possess camera / player
 ├─ Runtime input
 ├─ Runtime scripts
 ├─ Runtime physics
 ├─ Reset after stop
 └─ Keep changes option
```

---

# 27. Material Editor

```text
Material Editor
 ├─ Material preview sphere
 ├─ Base color
 ├─ Metallic
 ├─ Roughness
 ├─ Specular
 ├─ Transmission
 ├─ IOR
 ├─ Alpha mode
 ├─ Emissive
 ├─ Texture slots
 ├─ Normal map
 ├─ UV scale / offset
 ├─ Double-sided
 ├─ Material instance
 ├─ Save material
 └─ Apply to selection
```

Later:

```text
Node Material Editor
 ├─ Texture sample
 ├─ Color
 ├─ Scalar
 ├─ Multiply
 ├─ Add
 ├─ Normal map
 ├─ Output node
 └─ Compile material
```

---

# 28. Path-Traced Editor Behavior

```text
Editor Render Behavior
 ├─ Interactive mode while moving camera
 ├─ Interactive mode while transforming objects
 ├─ Reset accumulation after edit
 ├─ Resume convergence when idle
 ├─ Lower resolution while dragging
 ├─ Full resolution when idle
 ├─ Pause accumulation option
 ├─ Lock camera jitter option
 ├─ Reset denoiser history after major edits
 ├─ Refit TLAS after transform edit
 ├─ Rebuild BLAS after mesh import
 ├─ Rebuild environment CDF after HDRI change
 └─ Rebuild atmosphere LUT after atmosphere change
```

---

# 29. Scene Dirty Flags

```text
Dirty Flags
 ├─ TransformDirty
 ├─ MaterialDirty
 ├─ MeshDirty
 ├─ LightDirty
 ├─ CameraDirty
 ├─ EnvironmentDirty
 ├─ AtmosphereDirty
 ├─ FogDirty
 ├─ VolumeDirty
 ├─ PostProcessDirty
 ├─ TLASDirty
 ├─ BLASDirty
 ├─ DescriptorDirty
 ├─ AccumulationResetNeeded
 ├─ DenoiserHistoryResetNeeded
 └─ SceneSaveDirty
```

---

# 30. Logs / Console

## Log Panel

```text
Log Panel
 ├─ Info logs
 ├─ Warning logs
 ├─ Error logs
 ├─ Import logs
 ├─ Render logs
 ├─ Filter by type
 ├─ Search logs
 ├─ Clear logs
 ├─ Copy logs
 └─ Open log file
```

## Console

```text
Console
 ├─ Command input
 ├─ Command history
 ├─ Cvars
 ├─ Render commands
 ├─ Scene commands
 ├─ Import commands
 ├─ Profiling commands
 └─ Autocomplete
```

---

# 31. Profiling / Debug Tools

```text
Profiler Tools
 ├─ GPU frame time
 ├─ CPU frame time
 ├─ Trace pass time
 ├─ Denoise pass time
 ├─ TAA pass time
 ├─ Postprocess time
 ├─ TLAS build time
 ├─ BLAS build time
 ├─ Import time
 ├─ Memory usage
 ├─ Texture memory
 ├─ Buffer memory
 ├─ Acceleration structure memory
 ├─ Render graph viewer
 ├─ Frame timeline
 ├─ Resource lifetime report
 ├─ Screenshot diff
 └─ Regression test
```

---

# 32. Layout / Workspace System

```text
Layout System
 ├─ Docking layout save
 ├─ Default workspace
 ├─ Level editing workspace
 ├─ Lighting workspace
 ├─ Rendering workspace
 ├─ Material editing workspace
 ├─ Debugging workspace
 ├─ Animation workspace
 ├─ Reset layout
 ├─ Lock layout
 ├─ UI scale
 └─ Theme
```

---

# 33. Most Important Missing Features Now

Based on the current editor state and the import problem, these are the most urgent missing features:

```text
Critical Missing
1. Import as asset without replacing current scene
2. Import and place into current scene
3. Merge imported scene into current scene
4. Asset GUID database
5. Prefab generated from imported glTF / OBJ
6. Drag-drop asset placement
7. Full glTF / GLB material + texture support
8. OBJ importer
9. HDR / EXR / texture importer
10. Reimport system
11. Transform gizmo
12. Undo / redo
13. Parent-child hierarchy editing
14. Component-based inspector
15. Autosave / recovery
```

---

# 34. Recommended Implementation Order

## Phase 1 — Fix Import Architecture

```text
1. Rename current glTF behavior to “Import Scene as New Scene”
2. Add “Merge glTF into Current Scene”
3. Add “Import glTF as Asset”
4. Generate PrefabAsset from imported glTF hierarchy
5. Add drag prefab/model into viewport
6. Add OBJ importer
7. Add texture/HDR import path
8. Add reimport selected asset
```

## Phase 2 — Core Editor Interaction

```text
9. Transform gizmo
10. Undo / redo command system
11. Parent-child transform hierarchy
12. Entity context menu
13. Rename / duplicate / delete / group
```

## Phase 3 — Scene Persistence

```text
14. Proper .rtlevel scene format
15. Scene dirty state
16. Autosave / recovery
17. Save/load all components
18. Asset references by GUID
```

## Phase 4 — Asset Workflow

```text
19. Asset database
20. Content browser folder tree
21. Asset thumbnails/icons
22. Drag mesh into scene
23. Drag material onto object
24. Missing asset detection
```

## Phase 5 — UI Polish

```text
25. Viewport toolbar
26. Scene tab
27. Better inspector component blocks
28. Render World Settings tab
29. Timeline / log / console panels
30. Layout save/reset
```

## Phase 6 — Engine Workflow

```text
31. Project manager
32. Prefabs
33. Play / simulate mode
34. Cook assets
35. Build standalone viewer
36. Package project
```

---

# 35. Immediate Next Milestone

The next milestone should be:

```text
Editable Persistent Level + Real Asset Import
```

Definition:

```text
- Open a project
- Open a scene
- Import glTF / OBJ as reusable asset
- Generate mesh/material/texture/prefab assets
- Drag prefab into current scene
- Move/rotate/scale with gizmo
- Edit material/light/camera properties
- Save scene
- Close and reopen scene with all references preserved
- Undo/redo placement and transform edits
```

This milestone will make the editor behave like a real level editor instead of a renderer scene loader.
