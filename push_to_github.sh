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
    git commit -m "Initial commit: MySQL Clone"
fi

# Get default repository name from current directory
DEFAULT_REPO_NAME=$(basename "$(pwd)")
read -p "Enter GitHub repository name [default: $DEFAULT_REPO_NAME]: " REPO_NAME
REPO_NAME=${REPO_NAME:-$DEFAULT_REPO_NAME}

read -p "Make repository private? (y/N): " IS_PRIVATE
VISIBILITY="--public"
if [[ "$IS_PRIVATE" =~ ^[Yy]$ ]]; then
    VISIBILITY="--private"
fi

echo "Creating GitHub repository '$REPO_NAME'..."
gh repo create "$REPO_NAME" "$VISIBILITY" --source=. --remote=origin --push

echo "Successfully initialized and pushed your project to GitHub!"
