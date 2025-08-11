#pragma once

#include <string>
#include <vector>
#include <memory>
#include <unordered_map>

// Removed: #include "../shared/ScreencopyShared.hpp"
#include "../core/PortalManager.hpp"
#include "../helpers/Log.hpp"
#include "../helpers/MiscFunctions.hpp"

// Forward declaration for SSelectionData
struct SSelectionData;

class CPortalManager;

class CScreencopyPicker {
  public:
    CScreencopyPicker(CPortalManager* pPortalManager);
    SSelectionData promptForScreencopySelection();

  private:
    CPortalManager* m_pPortalManager;

    std::string sanitizeNameForWindowList(const std::string& name);
    std::string buildWindowList();
};