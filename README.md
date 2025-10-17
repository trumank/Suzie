# Suzie

Suzie is an Unreal Engine plugin that dynamically generates UHT (Unreal Header Tool) classes at editor runtime from dumped game class definitions. This allows working with game classes in Blueprint without having to generate headers and recompile if changed.

## How It Works

Suzie works in conjunction with [meatloaf](https://github.com/trumank/meatloaf), a tool that dumps class information from running Unreal Engine games. The workflow is:

1. Use meatloaf to dump class definitions from a running game into a JSON file
2. Place the JSON file in project's `Content/DynamicClasses` directory
3. Suzie reads this JSON file and generates all necessary classes at editor startup and during cooking
4. All native game classes become available in the Blueprint editor

## Supported Engine Versions

Suzie has been tested on Unreal Engine 5.3 through 5.6. Other versions may require minor tweaks (please submit a PR with fixes or create an issue showing errors).

## Installation & Usage

### Step 1: Obtain Game Dump

Use [meatloaf](https://github.com/trumank/meatloaf) to dump class definitions from target game into a `.json.gz` (or plain `.json`) file.

### Step 2: Set Up Unreal Project

1. Create a new C++ Unreal Engine project:
   - Use the same engine version as the target game
   - Use the same project name as the target game

2. Create the dynamic classes directory:
```bash
mkdir -p YourProject/Content/DynamicClasses
```

3. Copy the dumped JSON file:
```bash
cp output.json.gz YourProject/Content/DynamicClasses/
```

### Step 3: Install Suzie Plugin

1. Clone Suzie into project's Plugins directory:
```bash
cd YourProject/Plugins
git clone https://github.com/trumank/Suzie
```

2. Add Suzie to `.uproject` file:
```json
{
  "Plugins": [
    {
      "Name": "Suzie",
      "Enabled": true
    }
  ]
}
```

### Step 4: Build and Launch

1. Generate Visual Studio project files (right-click `.uproject` > Generate Visual Studio project files)
2. Open the `.sln` file in Visual Studio and build the solution
3. Launch the editor:
  - All native game structs, enums, and properties should now be available
