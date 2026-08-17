package com.jitong.im

import android.os.Bundle
import android.widget.Toast
import androidx.activity.ComponentActivity
import androidx.activity.compose.BackHandler
import androidx.activity.compose.setContent
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.ui.platform.LocalContext
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.viewmodel.compose.viewModel
import com.jitong.im.ui.ChatScreen
import com.jitong.im.ui.FriendListScreen
import com.jitong.im.ui.LoginScreen
import com.jitong.im.ui.MainViewModel
import com.jitong.im.ui.Screen
import com.jitong.im.ui.theme.JitongTheme

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContent {
            JitongTheme {
                AppNav()
            }
        }
    }
}

@Composable
private fun AppNav(vm: MainViewModel = viewModel()) {
    val screen by vm.screen.collectAsStateWithLifecycle()
    val context = LocalContext.current

    LaunchedEffect(Unit) {
        vm.toast.collect { Toast.makeText(context, it, Toast.LENGTH_LONG).show() }
    }

    when (screen) {
        Screen.Login -> LoginScreen(vm)
        Screen.FriendList -> FriendListScreen(vm)
        Screen.Chat -> {
            BackHandler { vm.backToFriends() }
            ChatScreen(vm)
        }
    }
}
