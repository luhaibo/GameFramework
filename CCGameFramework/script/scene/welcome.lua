local Scene = require('script.lib.core.scene')
local Gradient = require('script.lib.ui.gradient')
local Block = require('script.lib.ui.block')
local Text = require('script.lib.ui.text')

local modname = 'WelcomeScene'
local M = Scene:new()
_G[modname] = M
package.loaded[modname] = M

function M:init()
	UIExt.trace('Scene [Welcome page] init')
	-- INFO
	local info = UIExt.info()
	-- BG
	local bg = Block:new({
		color = '#111111',
		right = info.width,
		bottom = (info.height / 2) + 100
	})
	self.layers.bg = M:add(bg)
	UIExt.trace('Scene [Welcome page]: create background #' .. self.layers.bg.handle)
	-- BG2
	local bg2 = Gradient:new({
		color1 = '#111111',
		color2 = '#AAAAAA',
		direction = 1,
		top = info.height - bg.bottom,
		right = info.width,
		bottom = info.height
	})
	self.layers.bg2 = M:add(bg2)
	UIExt.trace('Scene [Welcome page]: create background2 #' .. self.layers.bg2.handle)
	-- TEXT
	local text = Text:new({
		color = '#EEEEEE',
		text = 'Hello world!',
		right = info.width,
		bottom = info.height
	})
	self.layers.text = M:add(text)
	UIExt.trace('Scene [Welcome page]: create text #' .. self.layers.text.handle)

	-- EVENT
	M:init_event()

	-- TIMER
	UIExt.set_timer(1, 3000)
end

function M:destroy()
	UIExt.trace('Scene [Welcome page] destroy')
	UIExt.clear_scene()
end

function M:init_event()
	self.handler = {
		[self.win_event.created] = function(this)
			UIExt.trace('Scene [Welcome page] Test created message!')
		end,
		[self.win_event.timer] = function(this, id)
			FlipScene('Time')
		end,
		[self.win_event.leftbuttondown] = function(this, x, y, flags, wheel)
		end,
		[self.win_event.char] = function(this, code, flags)
		end
	}
end

return M