#! /usr/bin/env python3
"""Script to garbage collect old amdvlk docker images created by the public CI on GitHub.

Requires python 3.9 or later.
"""

import argparse
import json
import logging
import subprocess
import sys

from collections import defaultdict
from typing import Any, Optional

def _run_cmd(cmd: list[str]) -> tuple[bool, str]:
    """
    Runs a shell command capturing its output.

    Args:
      cmd: List of strings to invoke as a subprocess

    Returns:
      Tuple: success, stdout. The first value is True on success.
    """
    logging.info('Running command: %s', ' '.join(cmd))
    result = subprocess.run(cmd, capture_output=True, check=False, text=True)
    if result.returncode != 0:
        logging.info('%s', result.stderr)
        return False, ''
    return True, result.stdout


def query_images(artifact_repository_url: str) -> Optional[list[dict[str, Any]]]:
    """
    Returns a list of JSON objects representing docker images found under
    |artifact_repository_url|, or None on error.

    Sample JSON object:
    {
      "createTime": "2022-07-11T20:20:23.577823Z",
      "package": "us-docker.pkg.dev/stadia-open-source/amdvlk-public-ci/amdvlk_release_gcc_assertions",
      "tags": "",
      "updateTime": "2022-07-11T20:20:23.577823Z",
      "version": "sha256:e101b6336fa78014e4008df59667dd84616dc8d1b60c2240f3246ab9a1ed6b20"
    }
    """
    ok, text = _run_cmd(['gcloud', 'artifacts', 'docker', 'images', 'list',
                         artifact_repository_url, '--format=json', '--quiet'])
    if not ok:
        return None
    return list(json.loads(text))


def find_images_to_gc(images: list[dict[str, Any]], num_last_to_keep) -> list[dict[str, Any]]:
    """
    Returns a subset of |images| that should be garbage collected. Preserves tagged
    images and also the most recent |num_last_to_keep| for each package.
    """
    package_to_images = defaultdict(list)
    for image in images:
        package_to_images[image['package']].append(image)

    to_gc = []
    for _, images in package_to_images.items():
        # Because the time format is ISO 8601, the lexicographic order is also chronological.
        images.sort(key=lambda x: x['createTime'])
        for image in images[:-num_last_to_keep]:
            if not image['tags']:
                to_gc.append(image)

    return to_gc


def delete_images(images: list[dict[str, Any]], dry_run: bool) -> None:
    """
    Deletes all |images| from the repository. When |dry_run| is True, synthesizes the delete
    commands and logs but does not execute them.
    """
    for image in images:
        image_path = image['package'] + '@' + image['version']
        cmd = ['gcloud', 'artifacts', 'docker', 'images', 'delete',
               image_path, '--quiet']
        if dry_run:
            logging.info('Dry run: %s', ' '.join(cmd))
            continue

        ok, _ = _run_cmd(cmd)
        if not ok:
            logging.warning('Failed to delete image:\n%s', image)


def main() -> int:
    logging.basicConfig(
        format='%(levelname)s %(asctime)s %(filename)s:%(lineno)d  %(message)s',
        level=logging.INFO)
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)

    parser.add_argument(
        '--dry-run',
        action='store_true',
        default=False,
        help='Do not delete or modify any docker images (default: %(default)s).')
    parser.add_argument(
        '--keep-last',
        default=8,
        help='The number of the most recent images to keep for each package (default: %(default)s).')
    parser.add_argument(
        '--repository-url',
        default='us-docker.pkg.dev/stadia-open-source/amdvlk-public-ci',
        help='The repository with docker images to garbage collect (default: %(default)s).'
    )
    args = parser.parse_args()

    all_images = query_images(args.repository_url)
    if all_images is None:
        logging.error('Failed to list docker images under \'%s\'', args.repository_url)
        return 1

    to_gc = find_images_to_gc(all_images, args.keep_last)
    delete_images(to_gc, args.dry_run)
    return 0


if __name__ == '__main__':
    sys.exit(main())
