/**
 * Export Ultralytics YOLO .pt -> ONNX for j2k_ch.dll (matches j2k_config.json ``yolo_onnx`` path).
 * Spawns ``python`` with a short script in %TEMP% (no committed .py in the repo).
 *
 * Usage:
 *   j2k_export_onnx.exe
 *   j2k_export_onnx.exe path\to\best.pt
 *   j2k_export_onnx.exe path\to\best.pt path\to\j2k_yolo.onnx
 */
#ifndef _WIN32
#include <iostream>
int main() {
    std::cerr << "j2k_export_onnx is Windows-only.\n";
    return 1;
}
#else
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

static fs::path repo_root_from_exe() {
    char buf[MAX_PATH]{};
    if (GetModuleFileNameA(nullptr, buf, MAX_PATH) == 0) {
        return fs::current_path();
    }
    const fs::path exe_dir = fs::path(buf).parent_path();
    const fs::path build_dll = exe_dir.parent_path();
    const fs::path cpp_dir = build_dll.parent_path();
    return cpp_dir.parent_path();
}

static bool write_temp_runner(const fs::path& tmp_py) {
    static const char kScript[] =
        "import os, shutil\n"
        "from pathlib import Path\n"
        "from ultralytics import YOLO\n"
        "pt, dst = os.environ['J2K_EXPORT_PT'], os.environ['J2K_EXPORT_OUT']\n"
        "YOLO(pt).export(format='onnx', imgsz=1280, simplify=True, opset=12)\n"
        "src = str(Path(pt).with_suffix('.onnx'))\n"
        "if not Path(src).is_file():\n"
        "    raise SystemExit(2)\n"
        "Path(dst).parent.mkdir(parents=True, exist_ok=True)\n"
        "shutil.copy2(src, dst)\n"
        "print('[j2k_export_onnx] Wrote', dst)\n";
    std::error_code ec;
    fs::create_directories(tmp_py.parent_path(), ec);
    std::ofstream f(tmp_py, std::ios::binary | std::ios::trunc);
    if (!f) {
        return false;
    }
    f.write(kScript, static_cast<std::streamsize>(sizeof(kScript) - 1));
    return f.good();
}

int main(int argc, char** argv) {
    const fs::path repo = repo_root_from_exe();
    fs::path pt = repo / "python/models/object.v4i.yolov11/ultralytics_train/weights/best.pt";
    fs::path out = repo / "python/models/j2k_yolo.onnx";

    if (argc >= 2 && argv[1] != nullptr && argv[1][0] != '\0') {
        pt = fs::absolute(fs::path(argv[1]));
    }
    if (argc >= 3 && argv[2] != nullptr && argv[2][0] != '\0') {
        out = fs::absolute(fs::path(argv[2]));
    }

    std::error_code ec;
    if (!fs::exists(pt, ec)) {
        std::cerr << "[j2k_export_onnx] Missing weights: " << pt.string() << "\n";
        std::cerr << "  Usage: j2k_export_onnx.exe [best.pt] [out.onnx]\n";
        return 1;
    }

    _putenv_s("J2K_EXPORT_PT", pt.string().c_str());
    _putenv_s("J2K_EXPORT_OUT", out.string().c_str());

    const fs::path tmp_py = fs::temp_directory_path() / "j2k_export_onnx_run.py";
    if (!write_temp_runner(tmp_py)) {
        std::cerr << "[j2k_export_onnx] Could not write temp script: " << tmp_py.string() << "\n";
        return 1;
    }

    std::string cmd = "python \"";
    cmd += tmp_py.string();
    cmd += "\"";
    const int rc = std::system(cmd.c_str());
    std::error_code ec2;
    fs::remove(tmp_py, ec2);
    if (rc != 0) {
        std::cerr << "[j2k_export_onnx] python exited with code " << rc << "\n";
        return (rc > 0 && rc < 256) ? rc : 1;
    }
    return 0;
}
#endif
