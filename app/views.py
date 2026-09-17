"""Shared templates without importing the running application."""
from fastapi.templating import Jinja2Templates
from app.config import get_settings

templates = Jinja2Templates(directory='templates')
templates.env.globals['root_path'] = get_settings().root_path + '/'
