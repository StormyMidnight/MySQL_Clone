#!/usr/bin/env bash
set -e

# Check for git
if ! command -v git &> /dev/null; then
    echo "Error: git is not installed."
    exit 1
fi

# Check for GitHub CLI (gh)
if ! command -v gh &> /dev/null; then
    echo "Error: GitHub CLI (gh) is not installed. Please install it and run 'gh auth login' first."
    exit 1
fi

# Initialize git repo if it doesn't exist
if [ ! -d ".git" ]; then
    echo "Initializing Git repository..."
    git init
    git branch -M main
else
    echo "Git repository already initialized."
fi

# Stage and commit files
echo "Staging project files..."
git add .

if git diff-index --quiet HEAD -- 2>/dev/null; then
    echo "No changes to commit."
else
    git commit -m "Update project files"
fi

# Prompt for the existing repository name or URL
DEFAULT_REPO_NAME=$(basename "$(pwd)")
read -p "Enter your GitHub username/organization and repository name (e.g., username/$DEFAULT_REPO_NAME): " REPO_PATH

if [ -z "$REPO_PATH" ]; then
    echo "Error: Repository path cannot be empty."
    exit 1
fi

# Set remote origin if it doesn't already exist
if git remote | grep -q "^origin$"; then
    echo "Remote 'origin' already exists."
else
    echo "Adding remote origin..."
    git remote add origin "https://github.com/$REPO_PATH.git"
fi

# Push to the existing repository
echo "Pushing to GitHub..."
git push -u origin main

echo "Successfully pushed your project to GitHub!"