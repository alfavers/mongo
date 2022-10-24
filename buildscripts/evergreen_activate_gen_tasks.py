#!/usr/bin/env python3
"""Activate an evergreen task in the existing build."""
import os
import sys
from typing import List, Optional

import click
import structlog
from pydantic.main import BaseModel

from evergreen.api import EvergreenApi, RetryingEvergreenApi

# Get relative imports to work when the package is not installed on the PYTHONPATH.
if __name__ == "__main__" and __package__ is None:
    sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# pylint: disable=wrong-import-position
from buildscripts.util.cmdutils import enable_logging
from buildscripts.util.fileops import read_yaml_file
from buildscripts.util.taskname import remove_gen_suffix

# pylint: enable=wrong-import-position

LOGGER = structlog.getLogger(__name__)

EVG_CONFIG_FILE = "./.evergreen.yml"
BURN_IN_TAGS = "burn_in_tags"
BURN_IN_TESTS = "burn_in_tests"


class EvgExpansions(BaseModel):
    """
    Evergreen expansions file contents.

    build_id: ID of build being run.
    version_id: ID of version being run.
    task_name: Name of task creating the generated configuration.
    burn_in_tag_buildvariants: Buildvariants to run burn_in_tags on.
    """

    build_id: str
    version_id: str
    task_name: str
    burn_in_tag_buildvariants: Optional[str] = None

    @classmethod
    def from_yaml_file(cls, path: str) -> "EvgExpansions":
        """Read the generation configuration from the given file."""
        return cls(**read_yaml_file(path))

    @property
    def task(self) -> str:
        """Get the task being generated."""
        return remove_gen_suffix(self.task_name)

    @property
    def burn_in_tag_buildvariants_list(self) -> List[str]:
        """Get the list of burn_in_tags buildvariants."""
        if self.burn_in_tag_buildvariants is None:
            return []
        return self.burn_in_tag_buildvariants.split()


def activate_task(expansions: EvgExpansions, evg_api: EvergreenApi) -> None:
    """
    Activate the given task in the specified build.

    :param expansions: Evergreen expansions file contents.
    :param evg_api: Evergreen API client.
    """
    if expansions.task == BURN_IN_TAGS:
        version = evg_api.version_by_id(expansions.version_id)
        for base_build_variant in expansions.burn_in_tag_buildvariants_list:
            build_variant = f"{base_build_variant}-required"
            try:
                build_id = version.build_variants_map[build_variant]
            except KeyError:
                LOGGER.warning(
                    "It is likely nothing to burn_in, so burn_in_tags build variant"
                    " was not generated. Skipping...", build_variant=build_variant)
                continue

            task_list = evg_api.tasks_by_build(build_id)

            for task in task_list:
                if task.display_name == BURN_IN_TESTS:
                    LOGGER.info("Activating task", task_id=task.task_id,
                                task_name=task.display_name)
                    evg_api.configure_task(task.task_id, activated=True)

    else:
        task_list = evg_api.tasks_by_build(expansions.build_id)
        for task in task_list:
            if task.display_name == expansions.task:
                LOGGER.info("Activating task", task_id=task.task_id, task_name=task.display_name)
                evg_api.configure_task(task.task_id, activated=True)


@click.command()
@click.option("--expansion-file", type=str, required=True,
              help="Location of expansions file generated by evergreen.")
@click.option("--evergreen-config", type=str, default=EVG_CONFIG_FILE,
              help="Location of evergreen configuration file.")
@click.option("--verbose", is_flag=True, default=False, help="Enable verbose logging.")
def main(expansion_file: str, evergreen_config: str, verbose: bool) -> None:
    """
    Activate the associated generated executions based in the running build.

    The `--expansion-file` should contain all the configuration needed to generate the tasks.
    \f
    :param expansion_file: Configuration file.
    :param evergreen_config: Evergreen configuration file.
    :param verbose: Use verbose logging.
    """
    enable_logging(verbose)
    expansions = EvgExpansions.from_yaml_file(expansion_file)
    evg_api = RetryingEvergreenApi.get_api(config_file=evergreen_config)

    activate_task(expansions, evg_api)


if __name__ == "__main__":
    main()  # pylint: disable=no-value-for-parameter
