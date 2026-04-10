// INSTRUCTIONS: Rename this file to 'node_registry.h' and generate your own secure keys.
#pragma once

// Explicit dynamic whitelist of authorized Edge Nodes
static const uint8_t authorizedEdgeNodes[][6] = {
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, // Node 1 (Example)
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}  // Node 2 (Example)
    // Add new nodes here separated by commas
};

// Calculate total nodes dynamically
static const int numAuthorizedNodes = sizeof(authorizedEdgeNodes) / sizeof(authorizedEdgeNodes[0]);