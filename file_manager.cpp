/*
 * Copyright (C) 2026 The LineageOS Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "file_manager.h"

#include <dirent.h>
#include <errno.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/unistd.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include <android-base/logging.h>
#include <android-base/stringprintf.h>
#include <fs_mgr.h>
#include <fs_mgr/roots.h>

#include "minui/minui.h"
#include "recovery_ui/device.h"
#include "recovery_ui/ui.h"
#include "recovery_utils/roots.h"

static constexpr const char* MOUNT_POINT = "/mnt/system";

// Hard limits for the built-in editor, to avoid hanging the recovery on huge files.
static constexpr off_t kMaxEditableBytes = 64LL * 1024 * 1024;  // 64 MB
static constexpr size_t kMaxEditableLines = 200000;

// Return true if the path refers to a directory (following symlinks).
static bool IsDirectory(const std::string& path) {
  struct stat st;
  if (stat(path.c_str(), &st) != 0) {
    return false;
  }
  return S_ISDIR(st.st_mode);
}

// Returns true on success, false on error.
static bool EnsureSystemMounted() {
  // Already mounted?
  struct stat st;
  if (stat(MOUNT_POINT, &st) == 0) {
    // /mnt/system is created by etc/init.rc; verify it is actually a mount point.
    bool is_mount = false;
    FILE* fp = fopen("/proc/mounts", "r");
    if (fp != nullptr) {
      char line[1024];
      while (fgets(line, sizeof(line), fp) != nullptr) {
        std::string entry = line;
        if (entry.find(MOUNT_POINT) != std::string::npos) {
          is_mount = true;
          // If already mounted read-only, try to remount rw.
          if (entry.find(" rw,") == std::string::npos && entry.find(" rw ") == std::string::npos) {
            mount(MOUNT_POINT, MOUNT_POINT, nullptr, MS_REMOUNT | MS_RDONLY, nullptr);
            mount(MOUNT_POINT, MOUNT_POINT, nullptr, MS_REMOUNT, nullptr);
          }
          break;
        }
      }
      fclose(fp);
    }
    if (is_mount) {
      return true;
    }
  }

  // Not mounted: mount the root partition there, like MOUNT_SYSTEM does.
  if (ensure_path_mounted_at(android::fs_mgr::GetSystemRoot(), MOUNT_POINT) == -1) {
    LOG(ERROR) << "Failed to mount " << MOUNT_POINT;
    return false;
  }

  // Make sure the filesystem is writable for deletion.
  if (mount(MOUNT_POINT, MOUNT_POINT, nullptr, MS_REMOUNT, nullptr) != 0) {
    LOG(WARNING) << "Failed to remount " << MOUNT_POINT << " rw: " << strerror(errno);
  }
  return true;
}

// Human readable file size: B / KB / MB / GB.
static std::string HumanSize(off_t size) {
  double s = static_cast<double>(size);
  char buf[32];
  if (s < 1024.0) {
    snprintf(buf, sizeof(buf), "%lld B", static_cast<long long>(size));
  } else if (s < 1024.0 * 1024.0) {
    snprintf(buf, sizeof(buf), "%.1f KB", s / 1024.0);
  } else if (s < 1024.0 * 1024.0 * 1024.0) {
    snprintf(buf, sizeof(buf), "%.1f MB", s / (1024.0 * 1024.0));
  } else {
    snprintf(buf, sizeof(buf), "%.1f GB", s / (1024.0 * 1024.0 * 1024.0));
  }
  return buf;
}

// One entry shown in the file manager list.
struct Entry {
  std::string display;  // text shown in the menu
  std::string real;     // real base name (no size suffix, no trailing '/')
  bool is_dir;
};

// Lists the contents of |path|. Directories come first, then files, each sorted
// lexicographically by real name. Directory entries get a trailing '/' and file
// entries get their human readable size appended. Returns false on error.
static bool ListDirectory(const std::string& path, std::vector<Entry>* out) {
  std::vector<Entry> dirs;
  std::vector<Entry> files;

  DIR* dir = opendir(path.c_str());
  if (dir == nullptr) {
    PLOG(ERROR) << "Failed to open directory " << path;
    return false;
  }

  struct dirent* de;
  while ((de = readdir(dir)) != nullptr) {
    std::string name = de->d_name;
    if (name == "." || name == "..") {
      continue;
    }
    std::string full = path + "/" + name;
    bool is_dir = de->d_type == DT_DIR;
    if (de->d_type == DT_UNKNOWN || de->d_type == DT_LNK) {
      is_dir = IsDirectory(full);
    }
    if (is_dir) {
      dirs.push_back({ name + "/", name, true });
    } else {
      std::string sz;
      struct stat fst;
      if (stat(full.c_str(), &fst) == 0) {
        sz = HumanSize(fst.st_size);
      }
      std::string display = name;
      if (!sz.empty()) {
        display += "  (" + sz + ")";
      }
      files.push_back({ display, name, false });
    }
  }
  closedir(dir);

  std::sort(dirs.begin(), dirs.end(),
            [](const Entry& a, const Entry& b) { return a.real < b.real; });
  std::sort(files.begin(), files.end(),
            [](const Entry& a, const Entry& b) { return a.real < b.real; });

  out->clear();
  out->push_back({ "../", "../", true });  // Always present as "go up".
  out->insert(out->end(), dirs.begin(), dirs.end());
  out->insert(out->end(), files.begin(), files.end());
  return true;
}

// Asks for confirmation, then deletes a single file (or directory tree).
static bool DeleteEntry(const std::string& path) {
  if (unlink(path.c_str()) == 0) {
    return true;
  }
  if (errno == EISDIR || errno == EPERM) {
    if (rmdir(path.c_str()) == 0) {
      return true;
    }
    // Slow recursive unlink for non-empty directories.
    DIR* dir = opendir(path.c_str());
    if (dir == nullptr) {
      return false;
    }
    struct dirent* de;
    bool ok = true;
    while ((de = readdir(dir)) != nullptr) {
      std::string name = de->d_name;
      if (name == "." || name == "..") {
        continue;
      }
      if (!DeleteEntry(path + "/" + name)) {
        ok = false;
      }
    }
    closedir(dir);
    if (ok && rmdir(path.c_str()) == 0) {
      return true;
    }
    return false;
  }
  return false;
}

static bool BrowseDirectory(const std::string& path, Device* device, RecoveryUI* ui);

// ---------------------------------------------------------------------------
// Text input helpers (USB keyboard).
// ---------------------------------------------------------------------------

static bool IsKeyDown(RecoveryUI* ui, int key) {
  return ui->IsKeyPressed(key);
}

// Map a USB keyboard keycode (US QWERTY layout) to a printable char.
// Returns '\0' if the key does not produce a printable character.
static char KeyToChar(int key, bool shift) {
  switch (key) {
    case KEY_1: return shift ? '!' : '1';
    case KEY_2: return shift ? '@' : '2';
    case KEY_3: return shift ? '#' : '3';
    case KEY_4: return shift ? '$' : '4';
    case KEY_5: return shift ? '%' : '5';
    case KEY_6: return shift ? '^' : '6';
    case KEY_7: return shift ? '&' : '7';
    case KEY_8: return shift ? '*' : '8';
    case KEY_9: return shift ? '(' : '9';
    case KEY_0: return shift ? ')' : '0';
    case KEY_MINUS: return shift ? '_' : '-';
    case KEY_EQUAL: return shift ? '+' : '=';
    case KEY_LEFTBRACE: return shift ? '{' : '[';
    case KEY_RIGHTBRACE: return shift ? '}' : ']';
    case KEY_SEMICOLON: return shift ? ':' : ';';
    case KEY_APOSTROPHE: return shift ? '"' : '\'';
    case KEY_GRAVE: return shift ? '~' : '`';
    case KEY_BACKSLASH: return shift ? '|' : '\\';
    case KEY_COMMA: return shift ? '<' : ',';
    case KEY_DOT: return shift ? '>' : '.';
    case KEY_SLASH: return shift ? '?' : '/';
    case KEY_SPACE: return ' ';
    // Letters (three evdev rows).
    case KEY_Q: return shift ? 'Q' : 'q';
    case KEY_W: return shift ? 'W' : 'w';
    case KEY_E: return shift ? 'E' : 'e';
    case KEY_R: return shift ? 'R' : 'r';
    case KEY_T: return shift ? 'T' : 't';
    case KEY_Y: return shift ? 'Y' : 'y';
    case KEY_U: return shift ? 'U' : 'u';
    case KEY_I: return shift ? 'I' : 'i';
    case KEY_O: return shift ? 'O' : 'o';
    case KEY_P: return shift ? 'P' : 'p';
    case KEY_A: return shift ? 'A' : 'a';
    case KEY_S: return shift ? 'S' : 's';
    case KEY_D: return shift ? 'D' : 'd';
    case KEY_F: return shift ? 'F' : 'f';
    case KEY_G: return shift ? 'G' : 'g';
    case KEY_H: return shift ? 'H' : 'h';
    case KEY_J: return shift ? 'J' : 'j';
    case KEY_K: return shift ? 'K' : 'k';
    case KEY_L: return shift ? 'L' : 'l';
    case KEY_Z: return shift ? 'Z' : 'z';
    case KEY_X: return shift ? 'X' : 'x';
    case KEY_C: return shift ? 'C' : 'c';
    case KEY_V: return shift ? 'V' : 'v';
    case KEY_B: return shift ? 'B' : 'b';
    case KEY_N: return shift ? 'N' : 'n';
    case KEY_M: return shift ? 'M' : 'm';
    default:
      return '\0';
  }
}

// Rename |path| to a new name typed on the USB keyboard.
static void RenameEntry(const std::string& path, RecoveryUI* ui) {
  std::string dir = path;
  size_t slash = dir.rfind('/');
  std::string current, new_name;
  if (slash != std::string::npos) {
    current = dir.substr(slash + 1);
    dir = dir.substr(0, slash);
  } else {
    current = dir;
    dir = "";
  }
  new_name = current;

  for (;;) {
    // Render the rename box on a blank screen.
    const GRFont* font = gr_sys_font();
    int cw, ch;
    gr_font_size(font, &cw, &ch);
    int fb_w = gr_fb_width();
    int fb_h = gr_fb_height();
    int margin = 4;
    int max_cols = (fb_w - 2 * margin) / cw;

    gr_color(0, 0, 0, 255);
    gr_clear();

    std::string title = "RENAME";
    gr_color(255, 255, 0, 255);
    gr_text(font, margin, margin, title.c_str(), true);

    std::string dirline = "Dir: " + dir;
    if ((int)dirline.size() > max_cols) dirline.resize(max_cols);
    gr_color(180, 180, 180, 255);
    gr_text(font, margin, margin + ch, dirline.c_str(), false);

    std::string nameline = "New name: " + new_name + "_";
    if ((int)nameline.size() > max_cols) nameline.resize(max_cols);
    gr_color(255, 255, 255, 255);
    gr_text(font, margin, margin + 3 * ch, nameline.c_str(), false);

    std::string hint = "[Enter] rename   [Esc/Back] cancel";
    gr_color(0, 200, 0, 255);
    gr_text(font, margin, fb_h - ch, hint.c_str(), false);

    gr_flip();

    RecoveryUI::InputEvent evt = ui->WaitInputEvent();
    if (evt.type() != RecoveryUI::EventType::KEY) {
      continue;
    }
    int key = evt.key();
    bool shift = IsKeyDown(ui, KEY_LEFTSHIFT) || IsKeyDown(ui, KEY_RIGHTSHIFT);

    char c = KeyToChar(key, shift);
    if (c != '\0') {
      new_name += c;
      continue;
    }

    switch (key) {
      case KEY_BACKSPACE:
        if (!new_name.empty()) new_name.pop_back();
        break;
      case KEY_ENTER: {
        if (new_name.empty() || new_name == current || new_name == "." || new_name == "..") {
          break;
        }
        if (new_name.find('/') != std::string::npos) {
          ui->Print("Rename: '/' not allowed.\n");
          break;
        }
        std::string new_path = dir + "/" + new_name;
        if (::rename(path.c_str(), new_path.c_str()) == 0) {
          ui->Print("Renamed to %s\n", new_path.c_str());
        } else {
          PLOG(ERROR) << "Failed to rename " << path << " -> " << new_path;
          ui->Print("Rename failed: %s\n", strerror(errno));
        }
        return;
      }
      case KEY_ESC:
      case KEY_BACK:
      default:
        return;
    }
  }
}

// ---------------------------------------------------------------------------
// Full screen text editor.
// ---------------------------------------------------------------------------

// Number of text lines that fit on the editor screen (head/foot reserved).
static int EditorVisibleRows() {
  const GRFont* font = gr_sys_font();
  int cw, ch;
  gr_font_size(font, &cw, &ch);
  int fb_h = gr_fb_height();
  int top_y = ch;
  int bottom_y = fb_h - ch;
  int rows = (bottom_y - top_y) / ch;
  return rows > 0 ? rows : 0;
}

static void DrawEditor(const std::string& path, const std::vector<std::string>& lines,
                       size_t scroll_top, size_t cursor_row, size_t cursor_col, bool dirty) {
  const GRFont* font = gr_sys_font();
  int cw, ch;
  gr_font_size(font, &cw, &ch);
  int fb_w = gr_fb_width();
  int fb_h = gr_fb_height();
  int margin = 4;
  int top_y = ch;
  int bottom_y = fb_h - ch;
  int max_cols = (fb_w - 2 * margin) / cw;
  int rows = EditorVisibleRows();

  gr_color(0, 0, 0, 255);
  gr_clear();

  // Top status line.
  std::string title = path + "  [-- " + (dirty ? "EDITED*" : "BUFFER") + " --]";
  if ((int)title.size() > max_cols) title.resize(max_cols);
  gr_color(255, 255, 255, 255);
  gr_text(font, margin, 0, title.c_str(), true);

  // Body lines.
  for (int i = 0; i < rows; i++) {
    size_t li = scroll_top + static_cast<size_t>(i);
    int y = top_y + i * ch;
    if (li >= lines.size()) {
      break;
    }
    std::string line = lines[li];
    if ((int)line.size() > max_cols) line.resize(max_cols);

    if (li == cursor_row) {
      gr_color(60, 60, 60, 255);
      gr_fill(margin, y, fb_w - margin, y + ch);
    }
    gr_color(255, 255, 255, 255);
    gr_text(font, margin, y, line.c_str(), false);

    if (li == cursor_row) {
      size_t c = cursor_col;
      if (c > line.size()) c = line.size();
      int cx = margin + static_cast<int>(c) * cw;
      if (cx >= fb_w - cw) cx = fb_w - cw;
      // Inverted block cursor.
      gr_color(255, 255, 255, 255);
      gr_fill(cx, y, cx + cw, y + ch);
      char chbuf[2] = { 0, 0 };
      if (c < line.size()) chbuf[0] = line[c];
      gr_color(0, 0, 0, 255);
      gr_text(font, cx, y, chbuf, false);
    }
  }

  // Bottom status line.
  std::string footer = android::base::StringPrintf(
      "Line %zu/%zu  Col %zu   [Esc: exit] [Enter: new line] [Tab] [Arrows]",
      cursor_row + 1, lines.size(), cursor_col + 1);
  if ((int)footer.size() > max_cols) footer.resize(max_cols);
  gr_color(0, 200, 0, 255);
  gr_text(font, margin, bottom_y, footer.c_str(), false);

  gr_flip();
}

static void InsertChar(std::string* line, size_t col, char c) {
  if (col > line->size()) col = line->size();
  line->insert(line->begin() + static_cast<long>(col), c);
}

static size_t LeadingWhitespace(const std::string& s) {
  size_t n = 0;
  while (n < s.size() && (s[n] == ' ' || s[n] == '\t')) {
    n++;
  }
  return n;
}

// Edit |path| with a full screen editor. Returns true if the caller should
// stop browsing (currently always false on clean exit).
static bool EditFile(const std::string& path, Device* device, RecoveryUI* ui) {
  struct stat st;
  if (stat(path.c_str(), &st) != 0) {
    ui->Print("Edit: cannot stat %s\n", path.c_str());
    return false;
  }
  if (!S_ISREG(st.st_mode)) {
    ui->Print("Edit: not a regular file: %s\n", path.c_str());
    return false;
  }
  if (st.st_size > kMaxEditableBytes) {
    ui->Print("Edit: file too large for editing (%s, limit 64.0 MB).\n"
              "Use Rename/Delete if needed.\n",
              HumanSize(st.st_size).c_str());
    return false;
  }
  mode_t orig_mode = st.st_mode & 07777;

  // Load the file into memory.
  std::vector<std::string> lines;
  {
    std::ifstream in(path);
    if (!in.good()) {
      ui->Print("Edit: failed to open %s\n", path.c_str());
      return false;
    }
    std::string l;
    while (std::getline(in, l)) {
      if (!l.empty() && l.back() == '\r') l.pop_back();
      lines.push_back(std::move(l));
      if (lines.size() > kMaxEditableLines) {
        ui->Print("Edit: too many lines (>%zu).\n", kMaxEditableLines);
        return false;
      }
    }
  }
  if (lines.empty()) {
    lines.push_back("");
  }

  size_t cursor_row = 0;
  size_t cursor_col = 0;
  size_t scroll_top = 0;
  bool dirty = false;

  for (;;) {
    DrawEditor(path, lines, scroll_top, cursor_row, cursor_col, dirty);

    RecoveryUI::InputEvent evt = ui->WaitInputEvent();
    if (evt.type() != RecoveryUI::EventType::KEY) {
      continue;
    }
    int key = evt.key();
    bool shift = IsKeyDown(ui, KEY_LEFTSHIFT) || IsKeyDown(ui, KEY_RIGHTSHIFT);

    if (key == KEY_ESC || key == KEY_BACK) {
      // Exit menu.
      std::vector<std::string> actions{ "Save and exit", "Exit without saving", "Cancel" };
      std::vector<std::string> headers{ path, dirty ? "  (unsaved changes)" : "" };
      size_t chosen = ui->ShowMenu(
          headers, actions, 0, true,
          std::bind(&Device::HandleMenuKey, device, std::placeholders::_1, std::placeholders::_2));
      if (chosen == static_cast<size_t>(RecoveryUI::KeyError::INTERRUPTED)) {
        continue;
      }
      if (chosen == Device::kGoBack || chosen == Device::kGoHome) {
        continue;
      }
      if (chosen == 0) {  // Save and exit.
        std::ofstream out(path, std::ios::trunc);
        if (!out.good()) {
          ui->Print("Edit: failed to save %s: %s\n", path.c_str(), strerror(errno));
          continue;
        }
        for (const auto& l : lines) {
          out << l << "\n";
        }
        out.close();
        chmod(path.c_str(), orig_mode);
        ui->Print("Saved %s (%zu lines)\n", path.c_str(), lines.size());
        return false;
      } else if (chosen == 1) {  // Exit without saving.
        ui->Print("Discarded changes to %s\n", path.c_str());
        return false;
      }
      // Cancel: fall through, keep editing.
      continue;
    }

    if (key == KEY_LEFTSHIFT || key == KEY_RIGHTSHIFT) {
      continue;  // handled via IsKeyDown at char time.
    }

    char c = KeyToChar(key, shift);
    if (c != '\0' && c != ' ') {
      InsertChar(&lines[cursor_row], cursor_col, c);
      cursor_col++;
      dirty = true;
      continue;
    }

    switch (key) {
      case KEY_SPACE:
        InsertChar(&lines[cursor_row], cursor_col, ' ');
        cursor_col++;
        dirty = true;
        break;

      case KEY_TAB: {
        // Insert 4 spaces as indentation.
        for (int i = 0; i < 4; i++) {
          InsertChar(&lines[cursor_row], cursor_col, ' ');
        }
        cursor_col += 4;
        dirty = true;
        break;
      }

      case KEY_LEFT:
        if (cursor_col > 0) {
          cursor_col--;
        } else if (cursor_row > 0) {
          cursor_row--;
          cursor_col = lines[cursor_row].size();
        }
        break;

      case KEY_RIGHT:
        if (cursor_col < lines[cursor_row].size()) {
          cursor_col++;
        } else if (cursor_row + 1 < lines.size()) {
          cursor_row++;
          cursor_col = 0;
        }
        break;

      case KEY_UP:
        if (cursor_row > 0) {
          cursor_row--;
          if (cursor_col > lines[cursor_row].size()) {
            cursor_col = lines[cursor_row].size();
          }
        }
        break;

      case KEY_DOWN:
        if (cursor_row + 1 < lines.size()) {
          cursor_row++;
          if (cursor_col > lines[cursor_row].size()) {
            cursor_col = lines[cursor_row].size();
          }
        }
        break;

      case KEY_HOME:
        cursor_col = 0;
        break;

      case KEY_END:
        cursor_col = lines[cursor_row].size();
        break;

      case KEY_PAGEUP: {
        int rows = EditorVisibleRows();
        if (rows <= 0) rows = 1;
        if (scroll_top >= static_cast<size_t>(rows) - 1) {
          scroll_top -= static_cast<size_t>(rows) - 1;
        } else {
          scroll_top = 0;
        }
        cursor_row = scroll_top;
        if (cursor_col > lines[cursor_row].size()) cursor_col = lines[cursor_row].size();
        break;
      }

      case KEY_PAGEDOWN: {
        int rows = EditorVisibleRows();
        if (rows <= 0) rows = 1;
        size_t step = static_cast<size_t>(rows) - 1;
        scroll_top = std::min(scroll_top + step, lines.size() - 1);
        cursor_row = scroll_top;
        if (cursor_col > lines[cursor_row].size()) cursor_col = lines[cursor_row].size();
        break;
      }

      case KEY_BACKSPACE: {
        if (cursor_col > 0) {
          lines[cursor_row].erase(cursor_col - 1, 1);
          cursor_col--;
          dirty = true;
        } else if (cursor_row > 0) {
          // Merge current line to the end of the previous one.
          cursor_col = lines[cursor_row - 1].size();
          lines[cursor_row - 1] += lines[cursor_row];
          lines.erase(lines.begin() + static_cast<long>(cursor_row));
          cursor_row--;
          dirty = true;
        }
        break;
      }

      case KEY_DELETE: {
        if (cursor_col < lines[cursor_row].size()) {
          lines[cursor_row].erase(cursor_col, 1);
          dirty = true;
        } else if (cursor_row + 1 < lines.size()) {
          lines[cursor_row] += lines[cursor_row + 1];
          lines.erase(lines.begin() + static_cast<long>(cursor_row + 1));
          dirty = true;
        }
        break;
      }

      case KEY_ENTER: {
        // Split line, carrying the indentation of the current line to the new one.
        size_t indent = LeadingWhitespace(lines[cursor_row]);
        std::string tail = lines[cursor_row].substr(cursor_col);
        lines[cursor_row].erase(cursor_col);
        std::string newline = std::string(indent, ' ') + tail;
        lines.insert(lines.begin() + static_cast<long>(cursor_row + 1), std::move(newline));
        cursor_row++;
        cursor_col = indent;
        dirty = true;
        break;
      }

      default:
        break;
    }

    // Keep the cursor visible: auto-scroll.
    int rows = EditorVisibleRows();
    if (rows > 0) {
      size_t vis = static_cast<size_t>(rows);
      if (cursor_row < scroll_top) scroll_top = cursor_row;
      if (cursor_row >= scroll_top + vis) scroll_top = cursor_row - vis + 1;
    }
  }
}

// ---------------------------------------------------------------------------
// Actions.
// ---------------------------------------------------------------------------

static bool HandleFileAction(const std::string& path, Device* device, RecoveryUI* ui) {
  std::vector<std::string> actions{ "Edit", "Rename", "Delete", "Cancel" };
  std::vector<std::string> headers{ path };
  size_t chosen = ui->ShowMenu(
      headers, actions, 0, true,
      std::bind(&Device::HandleMenuKey, device, std::placeholders::_1, std::placeholders::_2));

  if (chosen == static_cast<size_t>(RecoveryUI::KeyError::INTERRUPTED)) {
    return true;
  }
  if (chosen == Device::kGoBack || chosen == Device::kGoHome) {
    return false;
  }
  if (chosen == 0) {  // "Edit"
    EditFile(path, device, ui);
  } else if (chosen == 1) {  // "Rename"
    RenameEntry(path, ui);
  } else if (chosen == 2) {  // "Delete"
    std::vector<std::string> confirm{ " No", " Yea sure" };
    std::vector<std::string> confirm_header{
      "Delete " + path, "  THIS CAN NOT BE UNDONE!",
    };
    size_t yes = ui->ShowMenu(
        confirm_header, confirm, 0, true,
        std::bind(&Device::HandleMenuKey, device, std::placeholders::_1, std::placeholders::_2));
    if (yes == 1) {
      if (DeleteEntry(path)) {
        ui->Print("Deleted %s\n", path.c_str());
      } else {
        PLOG(ERROR) << "Failed to delete " << path;
        ui->Print("Failed to delete %s\n", path.c_str());
      }
    }
  }
  return false;
}

static bool HandleDirectoryAction(const std::string& path, Device* device, RecoveryUI* ui) {
  std::vector<std::string> actions{ "Open", "Rename", "Delete", "Cancel" };
  std::vector<std::string> headers{ path };
  size_t chosen = ui->ShowMenu(
      headers, actions, 0, true,
      std::bind(&Device::HandleMenuKey, device, std::placeholders::_1, std::placeholders::_2));

  if (chosen == static_cast<size_t>(RecoveryUI::KeyError::INTERRUPTED)) {
    return true;
  }
  if (chosen == Device::kGoBack || chosen == Device::kGoHome) {
    return false;
  }
  if (chosen == 0) {  // "Open"
    return BrowseDirectory(path, device, ui);
  } else if (chosen == 1) {  // "Rename"
    RenameEntry(path, ui);
  } else if (chosen == 2) {  // "Delete"
    std::vector<std::string> confirm{ " No", " Yea sure" };
    std::vector<std::string> confirm_header{
      "Delete " + path, "  THIS CAN NOT BE UNDONE!  (recursive)",
    };
    size_t yes = ui->ShowMenu(
        confirm_header, confirm, 0, true,
        std::bind(&Device::HandleMenuKey, device, std::placeholders::_1, std::placeholders::_2));
    if (yes == 1) {
      if (DeleteEntry(path)) {
        ui->Print("Deleted %s\n", path.c_str());
      } else {
        PLOG(ERROR) << "Failed to delete " << path;
        ui->Print("Failed to delete %s\n", path.c_str());
      }
    }
  }
  return false;
}

static bool BrowseDirectory(const std::string& path, Device* device, RecoveryUI* ui) {
  std::vector<Entry> entries;
  if (!ListDirectory(path, &entries)) {
    ui->Print("Failed to list %s\n", path.c_str());
    return true;
  }

  size_t chosen_item = 0;
  while (true) {
    std::vector<std::string> items;
    items.reserve(entries.size());
    for (const auto& e : entries) {
      items.push_back(e.display);
    }
    std::vector<std::string> headers{ "File manager", path };
    chosen_item = ui->ShowMenu(
        headers, items, chosen_item, true,
        std::bind(&Device::HandleMenuKey, device, std::placeholders::_1, std::placeholders::_2));

    if (chosen_item == static_cast<size_t>(RecoveryUI::KeyError::INTERRUPTED)) {
      return true;
    }
    if (chosen_item == Device::kGoHome) {
      return true;
    }
    if (chosen_item == Device::kGoBack || chosen_item == 0) {
      // ".." or Back pressed: go up one level.
      return false;
    }
    if (chosen_item >= entries.size()) {
      continue;
    }

    const Entry& entry = entries[chosen_item];
    if (entry.real == "../" || entry.is_dir) {
      if (entry.real == "../") {
        return false;
      }
      std::string new_path = path + "/" + entry.real;
      if (HandleDirectoryAction(new_path, device, ui)) {
        return true;
      }
    } else {
      std::string new_path = path + "/" + entry.real;
      if (HandleFileAction(new_path, device, ui)) {
        return true;
      }
    }

    // Refresh the listing (a file/dir may have been deleted or renamed).
    std::vector<Entry> fresh;
    if (ListDirectory(path, &fresh)) {
      entries = fresh;
    }
    if (chosen_item >= entries.size()) {
      chosen_item = 0;
    }
  }
}

int RunFileManager(Device* device) {
  RecoveryUI* ui = device->GetUI();
  if (!EnsureSystemMounted()) {
    ui->Print("File manager: mount failed.\n");
    return -1;
  }

  BrowseDirectory(MOUNT_POINT, device, ui);
  return 0;
}
