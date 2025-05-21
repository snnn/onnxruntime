#!/usr/bin/env python3
import os
import argparse
from pathlib import Path

def get_file_sizes(directory=".", top_n=10, recursive=False):
    """
    Finds the largest files in a given directory.

    Args:
        directory (str): The directory to search. Defaults to the current directory.
        top_n (int): The number of largest files to display. Defaults to 10.
        recursive (bool): If True, search recursively into subdirectories.
                          Defaults to False.

    Returns:
        list: A list of tuples, where each tuple contains (file_path, file_size_in_bytes).
              Returns an empty list if no files are found or the directory doesn't exist.
    """
    dir_path = Path(directory).resolve()
    if not dir_path.is_dir():
        print(f"Error: Directory '{dir_path}' not found.")
        return []

    file_sizes = []
    
    if recursive:
        file_iterator = (item for item in dir_path.rglob('*') if item.is_file())
    else:
        file_iterator = (item for item in dir_path.iterdir() if item.is_file())

    for item in file_iterator:
        try:
            if item.is_file():
                file_sizes.append((item, item.stat().st_size))
        except OSError as e:
            print(f"Error accessing {item}: {e}")
            continue 

    file_sizes.sort(key=lambda x: x[1], reverse=True)
    return file_sizes[:top_n]

def human_readable_size(size_bytes):
    """
    Converts a size in bytes to a human-readable format (KB, MB, GB, etc.).

    Args:
        size_bytes (int): Size in bytes.

    Returns:
        str: Human-readable size string.
    """
    if size_bytes == 0:
        return "0B"
    size_name = ("B", "KB", "MB", "GB", "TB", "PB", "EB", "ZB", "YB")
    i = 0
    num = float(size_bytes)
    while num >= 1024 and i < len(size_name) - 1:
        num /= 1024.0
        i += 1
    return f"{num:.2f}{size_name[i]}"

def delete_files_interactive(files_to_delete):
    """
    Interactively asks the user if they want to delete the listed files.

    Args:
        files_to_delete (list): A list of (Path_object, size) tuples for files.
    """
    if not files_to_delete:
        return

    print("\n--- File Deletion ---")
    print("The following files were listed:")
    for i, (file_path, size) in enumerate(files_to_delete):
        print(f"  [{i+1}] {human_readable_size(size).ljust(10)} {file_path}")

    while True:
        choice = input("Do you want to delete any of these files? (yes/no): ").strip().lower()
        if choice in ['yes', 'y']:
            break
        elif choice in ['no', 'n']:
            print("No files will be deleted.")
            return
        else:
            print("Invalid input. Please enter 'yes' or 'no'.")

    files_actually_deleted_count = 0
    space_freed = 0

    while True:
        delete_choice = input("Delete (a)ll listed, (s)elect specific files, or (c)ancel? (a/s/c): ").strip().lower()
        if delete_choice == 'c':
            print("Deletion cancelled.")
            break
        elif delete_choice == 'a':
            confirm_all = input(f"Are you sure you want to attempt to delete ALL {len(files_to_delete)} listed files? This action will proceed without individual file confirmations. (yes/no): ").strip().lower()
            if confirm_all == 'yes' or confirm_all == 'y':
                print(f"Proceeding to delete {len(files_to_delete)} file(s)...")
                for file_path, size in files_to_delete:
                    try:
                        file_path.unlink()
                        print(f"Deleted: {file_path}")
                        files_actually_deleted_count += 1
                        space_freed += size
                    except OSError as e:
                        print(f"Error deleting {file_path}: {e}")
                break 
            else:
                print("Deletion of all files cancelled.")
                continue # Allow user to choose another option or cancel
        elif delete_choice == 's':
            indices_to_delete = []
            while True:
                try:
                    raw_indices = input("Enter numbers of files to delete (e.g., 1 3 4) or 'done': ").strip()
                    if raw_indices.lower() == 'done':
                        break
                    if not raw_indices:
                        continue
                    indices_to_delete = [int(i)-1 for i in raw_indices.split()]
                    valid_indices = True
                    for index in indices_to_delete:
                        if not (0 <= index < len(files_to_delete)):
                            print(f"Error: Index {index+1} is out of range (1-{len(files_to_delete)}).")
                            valid_indices = False
                            break
                    if valid_indices:
                        break 
                except ValueError:
                    print("Invalid input. Please enter space-separated numbers or 'done'.")
            
            if indices_to_delete:
                print("\nSelected files for deletion:")
                # Use a set to handle duplicate inputs gracefully and then sort
                unique_sorted_indices = sorted(list(set(indices_to_delete)))
                selected_files_to_confirm = [files_to_delete[i] for i in unique_sorted_indices if 0 <= i < len(files_to_delete)]

                if not selected_files_to_confirm:
                    print("No valid files selected based on input.")
                    continue


                for file_path, size in selected_files_to_confirm:
                     print(f"  {human_readable_size(size).ljust(10)} {file_path}")
                
                confirm_selected = input(f"Confirm deletion of these {len(selected_files_to_confirm)} selected files? This action will proceed without individual file confirmations. (yes/no): ").strip().lower()
                if confirm_selected == 'yes' or confirm_selected == 'y':
                    print(f"Proceeding to delete {len(selected_files_to_confirm)} selected file(s)...")
                    for file_path, size in selected_files_to_confirm: # Iterate over the pre-filtered list
                        try:
                            file_path.unlink()
                            print(f"Deleted: {file_path}")
                            files_actually_deleted_count += 1
                            space_freed += size
                        except OSError as e:
                            print(f"Error deleting {file_path}: {e}")
                else:
                    print("Deletion of selected files cancelled.")
            else:
                print("No files selected for deletion.")
            break 
        else:
            print("Invalid choice. Please enter 'a', 's', or 'c'.")
    
    if files_actually_deleted_count > 0:
        print(f"\nSummary: Successfully deleted {files_actually_deleted_count} file(s), freeing {human_readable_size(space_freed)}.")
    else:
        if choice in ['yes', 'y']: # Only print if they initially wanted to delete
             print("\nNo files were ultimately deleted in this session.")


def main():
    parser = argparse.ArgumentParser(
        description="Find the biggest files in the current or specified directory. "
                    "Optionally, offers to delete them.",
        formatter_class=argparse.RawTextHelpFormatter
    )
    parser.add_argument(
        "-n", "--number",
        type=int,
        default=10,
        help="Number of biggest files to display (default: 10)"
    )
    parser.add_argument(
        "-d", "--directory",
        type=str,
        default=".",
        help="Directory to search (default: current directory)"
    )
    parser.add_argument(
        "-H", "--human-readable",
        action="store_true",
        help="Display file sizes in human-readable format (KB, MB, GB)"
    )
    parser.add_argument(
        "-r", "--recursive",
        action="store_true",
        help="Search recursively into subdirectories"
    )
    parser.add_argument(
        "--delete",
        action="store_true",
        help="After listing files, prompt for interactive deletion.\n"
             "USE WITH CAUTION. Confirmations are group-level."
    )

    args = parser.parse_args()

    search_path = Path(args.directory).resolve()
    print(f"Searching for top {args.number} largest files in '{search_path}'"
          f"{' recursively' if args.recursive else ''}...")

    biggest_files = get_file_sizes(args.directory, args.number, args.recursive)

    if not biggest_files:
        print(f"No files found in '{search_path}'{ ' or its subdirectories' if args.recursive else ''}"
              f" or the directory is empty/inaccessible.")
        return

    print(f"\nTop {len(biggest_files)} largest files found:")
    total_size_listed = 0
    for file_path, size in biggest_files:
        size_str = human_readable_size(size) if args.human_readable else f"{size} bytes"
        display_path = file_path 
        print(f"{size_str.ljust(15)} {display_path}")
        total_size_listed += size
    
    readable_total_size = human_readable_size(total_size_listed)
    print(f"\nTotal size of these {len(biggest_files)} listed files: {readable_total_size} ({total_size_listed} bytes)")

    if args.delete:
        delete_files_interactive(biggest_files)
    else:
        print("\nTo enable deletion, run the script with the --delete flag.")

if __name__ == "__main__":
    main()
