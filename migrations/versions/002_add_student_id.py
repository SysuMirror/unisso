"""add user student_id

Revision ID: 002
Revises: 001
Create Date: 2026-08-26 00:00:00

"""
from typing import Sequence, Union

from alembic import op
import sqlalchemy as sa


# revision identifiers, used by Alembic.
revision: str = "002"
down_revision: Union[str, None] = "001"
branch_labels: Union[str, Sequence[str], None] = None
depends_on: Union[str, Sequence[str], None] = None


def upgrade() -> None:
    op.add_column("users", sa.Column("student_id", sa.String(64), nullable=True))


def downgrade() -> None:
    op.drop_column("users", "student_id")
